"""Behavioral regression tests. Run on Linux/WSL with Python's standard library."""

import errno
import os
from pathlib import Path
import pty
import resource
import select
import signal
import subprocess
import tempfile
import time
import unittest


PROJECT = Path(__file__).resolve().parent


def stop_session(session):
    """Clean up only processes belonging to a session created by a test."""
    if Path("/proc").is_dir():
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            try:
                process = int(entry.name)
                if os.getsid(process) == session:
                    os.kill(process, signal.SIGKILL)
            except ProcessLookupError:
                pass
    else:
        try:
            os.killpg(session, signal.SIGKILL)
        except ProcessLookupError:
            pass


class Terminal:
    def __init__(self, binary, directory):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(directory)
            os.execv(binary, [binary])
        self.buffer = b""
        self.reaped = False

    def send(self, text):
        os.write(self.fd, text if isinstance(text, bytes) else text.encode())

    def until(self, marker, timeout=4):
        marker = marker.encode() if isinstance(marker, str) else marker
        deadline = time.monotonic() + timeout
        while marker not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise AssertionError(f"Terminal timed out waiting for {marker!r}: {self.buffer!r}")
            try:
                data = os.read(self.fd, 4096)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                data = b""
            if not data:
                raise AssertionError(f"Shell exited before {marker!r}: {self.buffer!r}")
            self.buffer += data
        end = self.buffer.index(marker) + len(marker)
        result, self.buffer = self.buffer[:end], self.buffer[end:]
        return result

    def wait(self, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            process, status = os.waitpid(self.pid, os.WNOHANG)
            if process:
                self.reaped = True
                return os.waitstatus_to_exitcode(status)
            time.sleep(0.01)
        raise AssertionError("Shell did not exit")

    def close(self):
        stop_session(self.pid)
        if not self.reaped:
            os.waitpid(self.pid, 0)
            self.reaped = True
        os.close(self.fd)


class MishTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_directory = tempfile.TemporaryDirectory(prefix="mish-test-build-")
        requested = os.environ.get("MISH_BINARY")
        cls.binary = str(Path(requested).resolve()) if requested else str(
            Path(cls.build_directory.name) / "mish"
        )
        if not requested:
            build = subprocess.run(
                ["g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                 "-g", str(PROJECT / "Main.cpp"), "-o", cls.binary],
                capture_output=True, text=True, timeout=40,
            )
            if build.returncode:
                cls.build_directory.cleanup()
                raise RuntimeError(build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.build_directory.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mish-test-")
        self.addCleanup(self.directory.cleanup)
        self.cwd = Path(self.directory.name)
        (self.cwd / "input.txt").write_text("hello\n")
        (self.cwd / "sub").mkdir()

    def run_mish(self, script="", *, args=None, piped=False, low_fd_limit=False):
        batch = self.cwd / "batch.mish"
        batch.write_text(script)
        argv = [self.binary] + ([] if piped else [str(batch)] if args is None else args)

        def limit_descriptors():
            resource.setrlimit(resource.RLIMIT_NOFILE, (32, 32))

        with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as error:
            started = time.monotonic()
            process = subprocess.Popen(
                argv, cwd=self.cwd,
                stdin=subprocess.PIPE if piped else subprocess.DEVNULL,
                stdout=output, stderr=error, start_new_session=True,
                preexec_fn=limit_descriptors if low_fd_limit else None,
            )
            try:
                process.communicate(script.encode() if piped else None, timeout=5)
                elapsed = time.monotonic() - started
            finally:
                stop_session(process.pid)
                process.wait(timeout=2)
            # Background processes can still share these file offsets while
            # exiting. pread captures from offset zero without racing a seek.
            stdout = os.pread(output.fileno(), os.fstat(output.fileno()).st_size, 0)
            stderr = os.pread(error.fileno(), os.fstat(error.fileno()).st_size, 0)
            return subprocess.CompletedProcess(
                argv, process.returncode, stdout.decode(), stderr.decode()
            ), elapsed

    def assert_ok(self, result, output=""):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, output)
        self.assertEqual(result.stderr, "")

    def terminal(self):
        terminal = Terminal(self.binary, self.cwd)
        self.addCleanup(terminal.close)
        terminal.until(" > ")
        return terminal

    def test_batch_output_has_no_banner(self):
        result, _ = self.run_mish("echo hello\n")
        self.assert_ok(result, "hello\n")

    def test_piped_commands_have_no_prompt(self):
        result, _ = self.run_mish("echo hello\nexit\n", piped=True)
        self.assert_ok(result, "hello\n")

    def test_blank_lines_and_final_line_without_newline(self):
        result, _ = self.run_mish("\n \t \n\techo hello")
        self.assert_ok(result, "hello\n")

    def test_tabs_and_crlf(self):
        result, _ = self.run_mish("echo\thello\r\nexit\r\n")
        self.assert_ok(result, "hello\n")

    def test_quotes_escapes_and_empty_arguments(self):
        result, _ = self.run_mish(
            """printf '[%s]\\n' "hello world" 'a|b&c<>;' one\\ two '' pre"fix"\n"""
        )
        self.assert_ok(result, "[hello world]\n[a|b&c<>;]\n[one two]\n[]\n[prefix]\n")

    def test_double_quote_escapes(self):
        result, _ = self.run_mish(r'''printf '[%s]\n' "a\"b" "a\\b" "\q"''' + "\n")
        self.assert_ok(result, '[a"b]\n[a\\b]\n[\\q]\n')

    def test_quoted_operator_is_a_filename(self):
        result, _ = self.run_mish('echo hello > ">"\ncat "<" < ">"\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.cwd / ">").read_text(), "hello\n")
        self.assertIn("<", result.stderr)

    def test_input_redirection(self):
        result, _ = self.run_mish("tr a-z A-Z < input.txt\n")
        self.assert_ok(result, "HELLO\n")

    def test_input_filename_is_not_a_command_argument(self):
        result, _ = self.run_mish("wc -c < input.txt\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "6")

    def test_output_redirection_keeps_following_arguments(self):
        result, _ = self.run_mish("echo before > out.txt after\n")
        self.assert_ok(result)
        self.assertEqual((self.cwd / "out.txt").read_text(), "before after\n")
        self.assertFalse((self.cwd / "after").exists())

    def test_combined_redirections_preserve_input_in_either_order(self):
        for command in ("cat > out.txt < input.txt", "cat < input.txt > out.txt"):
            with self.subTest(command=command):
                result, _ = self.run_mish(command + "\n")
                self.assert_ok(result)
                self.assertEqual((self.cwd / "input.txt").read_text(), "hello\n")
                self.assertEqual((self.cwd / "out.txt").read_text(), "hello\n")

    def test_repeated_redirections_are_applied_in_order(self):
        result, _ = self.run_mish("echo hello > first.txt > second.txt\n")
        self.assert_ok(result)
        self.assertEqual((self.cwd / "first.txt").read_text(), "")
        self.assertEqual((self.cwd / "second.txt").read_text(), "hello\n")

    def test_relative_absolute_and_hidden_output_paths(self):
        for path in ("./out.txt", ".hidden", str(self.cwd / "absolute.txt")):
            with self.subTest(path=path):
                result, _ = self.run_mish(f"echo hello > {path}\n")
                self.assert_ok(result)
                self.assertEqual((self.cwd / path).read_text(), "hello\n")

    def test_filename_with_spaces(self):
        result, _ = self.run_mish('echo hello > "two words.txt"\ncat < "two words.txt"\n')
        self.assert_ok(result, "hello\n")

    def test_missing_input_does_not_execute_command(self):
        result, _ = self.run_mish("touch marker < missing.txt\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing.txt", result.stderr)
        self.assertFalse((self.cwd / "marker").exists())

    def test_output_open_failure(self):
        result, _ = self.run_mish("echo hello > missing-directory/out.txt\n")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("missing-directory/out.txt", result.stderr)

    def test_multiple_pipes(self):
        result, _ = self.run_mish("echo hello|tr a-z A-Z|wc -c\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "6")

    def test_large_pipe_does_not_deadlock(self):
        result, _ = self.run_mish("seq 1 30000 | wc -l\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "30000")

    def test_early_pipeline_consumer_exits(self):
        result, _ = self.run_mish("yes | head -n 1\n")
        self.assert_ok(result, "y\n")

    def test_pipeline_status_is_last_command_status(self):
        for script, status in (("false | true\n", 0), ("true | false\n", 1)):
            with self.subTest(script=script):
                result, _ = self.run_mish(script)
                self.assertEqual(result.returncode, status, result.stderr)

    def test_unknown_command_status_and_diagnostic(self):
        result, _ = self.run_mish("definitely_not_a_mish_command\n")
        self.assertEqual(result.returncode, 127)
        self.assertIn("definitely_not_a_mish_command", result.stderr)
        self.assertIn(os.strerror(errno.ENOENT), result.stderr)
        self.assertNotIn("Bad file descriptor", result.stderr)

    def test_nonexecutable_command_status(self):
        (self.cwd / "not-executable").write_text("echo hello\n")
        result, _ = self.run_mish("./not-executable\n")
        self.assertEqual(result.returncode, 126)
        self.assertIn(os.strerror(errno.EACCES), result.stderr)

    def test_failed_command_status(self):
        result, _ = self.run_mish("false\n")
        self.assertEqual(result.returncode, 1)

    def test_signal_termination_status(self):
        result, _ = self.run_mish("sh -c 'kill -TERM $$'\n")
        self.assertEqual(result.returncode, 128 + signal.SIGTERM)

    def test_explicit_status_130_is_not_treated_as_a_signal(self):
        result, _ = self.run_mish("sh -c 'exit 130'\necho survived\n")
        self.assert_ok(result, "survived\n")

    def test_exit_preserves_last_status_and_stops_script(self):
        result, _ = self.run_mish("false\n\nexit\necho should-not-run\n")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")

    def test_runtime_error_does_not_prevent_next_command(self):
        result, _ = self.run_mish("missing_mish_command\necho recovered\n")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "recovered\n")
        self.assertIn("missing_mish_command", result.stderr)

    def test_missing_batch_file(self):
        result, _ = self.run_mish(args=["missing-script.mish"])
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("missing-script.mish", result.stderr)

    def test_extra_shell_arguments(self):
        result, _ = self.run_mish(args=["one", "two"])
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("Usage:", result.stderr)

    def test_invalid_syntax_is_rejected_before_side_effects(self):
        invalid = (
            "&", "&&", "|", "touch marker |", "touch marker || wc",
            "touch marker | | wc", "touch marker & & echo bad",
            "touch marker & | wc", "touch marker |  & wc",
            "touch marker >", "touch marker > > out.txt",
            "touch marker <", '> "file"', "touch marker ; echo bad",
            'touch marker "unfinished', "touch marker \\",
            "touch marker & echo ok |",
        )
        for command in invalid:
            with self.subTest(command=command):
                result, _ = self.run_mish(command + "\n")
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("syntax error", result.stderr)
                self.assertNotIn("Success", result.stderr)
                self.assertFalse((self.cwd / "marker").exists())

    def test_batch_syntax_error_stops_remaining_lines(self):
        result, _ = self.run_mish("&\necho should-not-run\n")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")

    def test_cd_changes_parent_directory(self):
        result, _ = self.run_mish("cd sub\npwd\n")
        self.assert_ok(result, str(self.cwd / "sub") + "\n")

    def test_cd_argument_errors(self):
        for command in ("cd", "cd sub extra"):
            with self.subTest(command=command):
                result, _ = self.run_mish(command + "\n")
                self.assertEqual(result.returncode, 2)
                self.assertNotIn("Success", result.stderr)

    def test_failed_cd_status(self):
        result, _ = self.run_mish("cd missing-directory\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing-directory", result.stderr)

    def test_builtin_redirection_and_stream_restoration(self):
        result, _ = self.run_mish("cd . > out.txt\necho restored\n")
        self.assert_ok(result, "restored\n")
        self.assertEqual((self.cwd / "out.txt").read_text(), "")

    def test_builtin_does_not_run_when_redirection_fails(self):
        result, _ = self.run_mish("cd sub < missing.txt\npwd\n")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, str(self.cwd) + "\n")
        self.assertIn("missing.txt", result.stderr)

    def test_builtin_in_pipeline_does_not_change_parent(self):
        result, _ = self.run_mish("cd sub | cat\npwd\n")
        self.assert_ok(result, str(self.cwd) + "\n")

    def test_exit_in_pipeline_does_not_exit_parent(self):
        result, _ = self.run_mish("exit | cat\necho still-running\n")
        self.assert_ok(result, "still-running\n")

    def test_environment_assignment_is_inherited(self):
        result, _ = self.run_mish('MISH_TEST="hello world"\nprintenv MISH_TEST\n')
        self.assert_ok(result, "hello world\n")

    def test_assignment_does_not_silently_discard_another_command(self):
        result, _ = self.run_mish("MISH_TEST=hello echo discarded\n")
        self.assertEqual(result.returncode, 2)
        self.assertIn("standalone", result.stderr)

    def test_executable_with_equals_in_path(self):
        executable = self.cwd / "test=program"
        executable.write_text("#!/bin/sh\necho hello\n")
        executable.chmod(0o755)
        result, _ = self.run_mish("./test=program\n")
        self.assert_ok(result, "hello\n")

    def test_background_command_does_not_block_following_line(self):
        result, elapsed = self.run_mish("sleep 2 &\necho ready\n")
        self.assert_ok(result, "ready\n")
        self.assertLess(elapsed, 1.0)

    def test_background_pipeline_does_not_block(self):
        result, elapsed = self.run_mish("sleep 2 | cat &\necho ready\n")
        self.assert_ok(result, "ready\n")
        self.assertLess(elapsed, 1.0)

    def test_background_builtin_does_not_change_parent(self):
        result, _ = self.run_mish("cd sub & pwd\n")
        self.assert_ok(result, str(self.cwd) + "\n")

    def test_background_cat_does_not_consume_command_input(self):
        result, _ = self.run_mish("cat &\necho ready\n", piped=True)
        self.assert_ok(result, "ready\n")

    def test_repeated_builtin_redirections_do_not_leak_descriptors(self):
        result, _ = self.run_mish(
            "cd . > out.txt\n" * 100 + "echo ready\n", low_fd_limit=True
        )
        self.assert_ok(result, "ready\n")

    def test_exec_does_not_inherit_internal_descriptors(self):
        probe = self.cwd / "fds.py"
        probe.write_text(
            "import os\n"
            "extra = []\n"
            "for name in os.listdir('/proc/self/fd'):\n"
            "    fd = int(name)\n"
            "    try:\n"
            "        os.fstat(fd)\n"
            "    except OSError:\n"
            "        continue\n"
            "    if fd > 2:\n"
            "        extra.append(fd)\n"
            "print(sorted(extra))\n"
        )
        result, _ = self.run_mish("python3 fds.py | cat\n")
        self.assert_ok(result, "[]\n")

    def test_ctrl_c_interrupts_foreground_command_and_preserves_shell(self):
        terminal = self.terminal()
        terminal.send("sh -c 'echo RUNNING; exec sleep 10'\n")
        terminal.until("RUNNING\r\n")
        terminal.send(b"\x03")
        terminal.until(" > ")
        terminal.send("echo SURVIVED\n")
        terminal.until("\r\nSURVIVED\r\n")
        terminal.until(" > ")
        terminal.send("exit\n")
        self.assertEqual(terminal.wait(), 0)

    def test_ctrl_c_at_prompt_preserves_shell(self):
        terminal = self.terminal()
        terminal.send(b"\x03")
        terminal.until(" > ")
        terminal.send("echo SURVIVED\n")
        terminal.until("\r\nSURVIVED\r\n")
        terminal.until(" > ")
        terminal.send("exit\n")
        self.assertEqual(terminal.wait(), 0)

    def test_original_three_line_welcome_banner(self):
        terminal = Terminal(self.binary, self.cwd)
        self.addCleanup(terminal.close)
        output = terminal.until(" > ")
        self.assertTrue(output.startswith(
            b"*******************************************\r\n"
            b"       WELCOME TO MISH SHELL\r\n"
            b"*******************************************\r\n"
        ), output)
        terminal.send("exit\n")
        self.assertEqual(terminal.wait(), 0)

    def test_interactive_syntax_error_preserves_shell(self):
        terminal = self.terminal()
        terminal.send("&\n")
        terminal.until("syntax error")
        terminal.until(" > ")
        terminal.send("echo SURVIVED\n")
        terminal.until("\r\nSURVIVED\r\n")
        terminal.until(" > ")
        terminal.send("exit\n")
        self.assertEqual(terminal.wait(), 0)

    @unittest.skipUnless(Path("/proc").is_dir(), "requires Linux /proc")
    def test_background_children_are_reaped_while_waiting_for_input(self):
        terminal = self.terminal()
        terminal.send("true &\n")
        terminal.until(" > ")
        children_file = Path(f"/proc/{terminal.pid}/task/{terminal.pid}/children")
        deadline = time.monotonic() + 3
        while children_file.read_text().strip() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(children_file.read_text().strip(), "")
        terminal.send("exit\n")
        self.assertEqual(terminal.wait(), 0)


if __name__ == "__main__":
    unittest.main()
