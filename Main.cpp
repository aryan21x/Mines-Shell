#include "mish.h"

#include <cerrno>
#include <cctype>
#include <csignal>
#include <poll.h>


/*
    isFile records whether we are reading from a script file.
    interactive() sets it to false and nonInteractive() sets it to true.
    The input loops keep interactive errors at the prompt and stop scripts
    on syntax errors, while lastStatus records the command's result.
*/
bool isFile;

// The original shell functions below share their last result and error state.
namespace {
int lastStatus = 0;
bool exitRequested = false;
bool syntaxError = false;
bool foregroundInterrupted = false;
vector<pid_t> backgroundPids;
vector<int> signalPipe(2, -1);
volatile sig_atomic_t signalWriteFd = -1;
volatile sig_atomic_t interrupted = 0;

struct shellToken {
    string text;
    size_t start;
    size_t end;
    bool isOperator;
};

enum class ReadResult { Line, End, Interrupted, Error };

void reportError(const string& context, int error);
bool initializeSignals();
void shutdownSignals();
void resetChildSignals();
void reapBackgroundChildren();
void drainSignalPipe();
bool duplicateTo(int source, int target);
bool createPipe(vector<int>& descriptors, bool nonblocking = false);
void closePipes(const vector<vector<int>>& pipes);
int waitForChild(pid_t child);
ReadResult readLine(int input, string& line);
bool readShellInput(int input, string& line, bool terminal);
bool readTokens(const string& input, vector<shellToken>& tokens, string& error);
bool isAssignment(const string& word);
bool isInbuiltCommand(const vector<string>& tokens);
bool applyRedirections(const commandsToExecute& command);
int executeParentBuiltin(const commandsToExecute& command);
}


/**
 * @brief The main function for Mish Shell.
 *
 * This function serves as the entry point for Mish Shell. The shell can
 * run in two states, interactive or non-interactive. It determines
 * whether the shell should run in interactive mode or execute a script
 * based on the command line arguments.
 * If the number of arguments is greater than 2 then it's an error.
 *  If it is in non-interective, then argv[1] is the name of the script file
 *
 * @param argc The number of command line arguments.
 * @param argv An array of strings representing the command line arguments.
 *
 * @return An integer indicating the exit status of the program.
 */

int main(int argc, char *argv[])
{
    if (!initializeSignals()) return 1;

    // Check if arguments were passed to the shell to run commands from a file
    if (argc == 1)
    {
        // If no arguments were passed. Running in interactive mode
        if (isatty(STDIN_FILENO))
        {
            cout << "*******************************************" << endl;
            cout << "       WELCOME TO MISH SHELL" << endl;
            cout << "*******************************************" << endl;
        }

        interactive();
    }
    else if (argc == 2)
    {
        // If 1 argument was passed. Running in non-interactive mode with a
        //script. where argv[1] is the the name of the script file
        // Batch mode leaves stdout for the commands' output.
        nonInteractive(argv[1]);
    }
    else if (argc > 2)
    {
        // Invalid number of arguments so print an error and exit
        cerr << "Usage: " << argv[0] << " [script]" << endl;
        lastStatus = 2;
    }

    // Exit the program
    shutdownSignals();
    return lastStatus;
}



/**
 * @brief Prints the current working directory as a prompt.
 *
 * This function retrieves the current working directory using getcwd function
 * and prints it as a prompt followed by ' > '. This function is from unistd.h
 */
void printPrompt() {
    char cwd[PATH_MAX];

    // Attempt to get the current working directory
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        cout << cwd << " > " << flush;
    }
}


/**
 * @brief Implements an interactive shell.
 *
 * This function creates an interactive shell that continuously reads user input
 * until EOF or the exit built-in. It displays a prompt, processes user input, and
 * repeats the process until the exit command is given. It also updates the isFile
 * variable
 */
void interactive() {
    string input;

    // Not a file if it is an interactive call.
    isFile = false;
    bool terminal = isatty(STDIN_FILENO);

    // Display the current working directory as the initial prompt.
    if (terminal) printPrompt();

    // Read until EOF or the exit built-in asks the shell to stop.
    // readShellInput also wakes for Ctrl+C and completed background children.
    while (!exitRequested && readShellInput(STDIN_FILENO, input, terminal)) {
        // Skip processing if the input is empty.
        if (input.empty()) {
            if (terminal) printPrompt();
            continue;
        }

        /* Process the user input.
         * This function calls reduceSpacesAndTrim, generateTokens, and
         * executeCommands to process commands, redirections, and pipes.
         */
        processInput(input);

        // Piped input uses batch error behavior, without printing a prompt.
        if (exitRequested || (!terminal && (syntaxError || foregroundInterrupted))) {
            break;
        }
        // Display the prompt for the next input.
        if (terminal) printPrompt();
    }
}


/**
 * @brief Processes input from a non-interactive source (file).
 *
 * This function reads input from a specified file, processes each line using
 * the processInput() function, and then returns. It is intended for batch processing
 * of commands stored in a file. It also updates the isFile variable to true
 *
 * @param fileName The name of the input file to be processed.
 */
void nonInteractive(string fileName)
{
    int fin = -1;
    // Input from a file if it is nonInteractive.
    isFile = true;

    // Open the script file; a failed open must report a nonzero status.
    if (!openInput(fin, fileName))
    {
        lastStatus = 1;
        return;
    }

    string input;
    // Read each line from the input file and process using processInput().
    while (!exitRequested && readShellInput(fin, input, false)) {
        if (input.empty()) {
            continue;
        }

        /* Process the user input.
         * This function calls reduceSpacesAndTrim, generateTokens, and
         * executeCommands, just as interactive mode does.
         */
        processInput(input);
        if (syntaxError || foregroundInterrupted) {
            break;
        }
    }
    close(fin);
}
/**
 * @brief Processes an input command.
 *
 * @param input The raw input command to be processed.
 *
 * @details This function takes an input command and processes it to identify
 * individual commands and their parameters. It then executes commands
 * in the background or through pipes based on the '&' or '|' symbols.
 *
 * @note The input command is expected to be a string containing one or more
 *       commands separated by '&' or '|' symbols for background execution or a pipeline, respectively.
 *
 * @see executeCommands()
 *
 * @example
 *   To process a command like "command1 & command2", the input should be:
 *   @code
 *   processInput("command1 & command2");
 *   @endcode
 *
 *   The function will identify "command1" and "command2" as separate commands
 *   and start the first in the background.
 */

void processInput(string input)
{
    // Keep track of the commands, tokens, and file redirections.
    int parallelCommandCount = 1;
    vector<string> tokens;
    string redirectedOutputFileName;
    string redirectedInputFileName;

    // Normalize spaces outside quotes and validate the entire line first.
    // Invalid syntax must not execute a command or open a redirection file.
    input = reduceSpacesAndTrim(input);
    if (input.empty())
    {
        return;
    }

    vector<shellToken> brokenInput;
    string error;
    if (!readTokens(input, brokenInput, error)) return;

    // Count actual separators, ignoring | and & inside quoted arguments.
    // A trailing & backgrounds the last command; it adds no empty command.
    for (size_t i = 0; i < brokenInput.size(); i++)
    {
        if (brokenInput[i].isOperator &&
            (brokenInput[i].text == "&" || brokenInput[i].text == "|") &&
            i + 1 < brokenInput.size())
        {
            parallelCommandCount += 1;
        }
    }

    // Create a vector of commandsToExecute objects, as in the original shell.
    vector<commandsToExecute> commands(parallelCommandCount);
    for (int i = 0; i < parallelCommandCount; i++)
    {
        commands[i].redirectOutputToFile = false;
        commands[i].redirectedInputFromFile = false;
        commands[i].isPipeEnd = false;
        commands[i].isPipeStart = false;
    }

    size_t tokenLocation = 0;
    size_t inputStart = 0;
    int pipelineStart = 0;
    // Process and configure the properties of each command.
    for (int i = 0; i < parallelCommandCount; i++)
    {
        while (tokenLocation < brokenInput.size() &&
               !(brokenInput[tokenLocation].isOperator &&
                 (brokenInput[tokenLocation].text == "|" ||
                  brokenInput[tokenLocation].text == "&")))
        {
            tokenLocation++;
        }

        size_t loc = tokenLocation < brokenInput.size()
            ? brokenInput[tokenLocation].start : input.size();
        string tempInput = input.substr(inputStart, loc - inputStart);

        // Generate the command's arguments and consume each redirection's
        // next filename, without dropping arguments after that filename.
        generateTokens(tempInput, tokens, redirectedOutputFileName,
                       redirectedInputFileName, &commands[i].redirections);
        if (syntaxError) return;
        commands[i].tokens = tokens;
        commands[i].redirectOutputFileName = redirectedOutputFileName;
        commands[i].redirectedInputFileName = redirectedInputFileName;
        for (const auto& redirection : commands[i].redirections)
        {
            if (redirection.isInput) commands[i].redirectedInputFromFile = true;
            else commands[i].redirectOutputToFile = true;
        }

        if (tokenLocation < brokenInput.size())
        {
            if (brokenInput[tokenLocation].text == "|")
            {
                // Mark the writing command and the reading command of a pipe.
                commands[i].isPipeStart = true;
                commands[i + 1].isPipeEnd = true;
            }
            else
            {
                // An & applies to the entire preceding pipeline.
                for (int j = pipelineStart; j <= i; j++)
                {
                    commands[j].isBackground = true;
                }
                pipelineStart = i + 1;
            }
            inputStart = brokenInput[tokenLocation].end;
            tokenLocation++;
        }
        tokens.clear();
    }

    // Execute the processed commands and preserve their result for main/exit.
    lastStatus = executeCommands(commands);
    commands.clear();
}


/**
 * @brief Reduces spaces and trims a given input string.
 *
 * This function validates and normalizes input without changing quoted arguments:
 * - Removes trailing spaces from the end of the input string.
 * - Breaks the input string into individual tokens, considering special characters
 *   such as '&', '|', '>', '<' as separate tokens.
 * - Skips consecutive spaces in the input.
 * - Checks for valid input/output redirection and pipe commands.
 * - Rejects empty commands without executing any part of the line.
 *
 * @param input The input string to be processed.
 * @return A modified string after reducing spaces, trimming, and handling special characters.
 *
 *
 *   string processedInput = reduceSpacesAndTrim("command1 & command2");
 *   @endcode
 *   The resulting 'processedInput' may be "command1 & command2" without extra spaces.
 */
string reduceSpacesAndTrim(string input)
{
    string result; // The final processed string.
    vector<string> brokenString; // Raw tokens retain their quotes and escapes.
    vector<shellToken> parsedTokens;
    string error;
    syntaxError = false;

    // Tokenize spaces, tabs, and operators, keeping quoted text together.
    if (!readTokens(input, parsedTokens, error))
    {
        cerr << "mish: syntax error: " << error << endl;
        syntaxError = true;
        lastStatus = 2;
        return "";
    }
    if (parsedTokens.empty()) return "";
    for (const auto& token : parsedTokens)
    {
        brokenString.push_back(input.substr(token.start, token.end - token.start));
    }

    // Check input/output redirection and pipe/parallel command syntax.
    bool hasCommand = false;
    for (size_t i = 0; i < parsedTokens.size(); i++)
    {
        if (!parsedTokens[i].isOperator)
        {
            hasCommand = true;
            continue;
        }
        const string& token = parsedTokens[i].text;
        if (token == ">" || token == "<")
        {
            // A filename may begin with / or . or be quoted. What matters
            // is that it is a word, rather than another operator.
            if (i + 1 == parsedTokens.size() || parsedTokens[i + 1].isOperator)
            {
                error = "expected a filename after '" + token + "'";
                break;
            }
            i++; // A redirection filename is not the command itself.
        }
        else
        {
            // Reject empty commands, including consecutive | or & tokens.
            if (!hasCommand)
            {
                error = "expected a command before '" + token + "'";
                break;
            }
            hasCommand = false;
        }
    }
    if (error.empty() && !hasCommand &&
        !(parsedTokens.back().isOperator && parsedTokens.back().text == "&"))
    {
        error = "expected a command";
    }
    if (!error.empty())
    {
        cerr << "mish: syntax error: " << error << endl;
        syntaxError = true;
        lastStatus = 2;
        return "";
    }

    // Reconstruct the corrected string. Preserve the raw quotes/escapes
    // so generateTokens can distinguish a literal "|" from a pipe.
    for (size_t i = 0; i < brokenString.size(); i++)
    {
        result += brokenString[i];
        if (i + 1 < brokenString.size()) result += " ";
    }

    // The exit built-in is executed later so redirections and status work.
    return result;
}

/**
 * @brief Generates tokens from the input string
 *
 * This function parses quote-aware words from the command. It also
 * identifies redirection of input and output if specified in the input string.
 *
 * @param input The input string to be tokenized.
 * @param tokens A reference to a vector of strings to store the generated tokens.
 * @param redirectedOutputFileName A reference to a string to store the filename if output is redirected.
 * @param redirectedInputFileName A reference to a string to store the filename if input is redirected.
 * @param redirections Optional list preserving the order of all file redirections.
 */
void generateTokens(string input, vector<string> &tokens, string &redirectedOutputFileName, string &redirectedInputFileName,
                    vector<fileRedirection>* redirections)
{
    tokens.clear();
    redirectedOutputFileName = "";
    redirectedInputFileName = "";
    if (redirections) redirections->clear();

    vector<shellToken> brokenInput;
    string error;
    if (!readTokens(input, brokenInput, error))
    {
        cerr << "mish: syntax error: " << error << endl;
        syntaxError = true;
        lastStatus = 2;
        return;
    }

    // Generate arguments while keeping quoted/escaped words together.
    for (size_t i = 0; i < brokenInput.size(); i++)
    {
        if (brokenInput[i].isOperator)
        {
            // Redirection consumes exactly one filename, not the rest
            // of the command. Preserve ordering for repeated redirects.
            bool isRedirectInput = brokenInput[i].text == "<";
            if (i + 1 == brokenInput.size() || brokenInput[i + 1].isOperator)
            {
                syntaxError = true;
                lastStatus = 2;
                return;
            }
            string fileName = brokenInput[++i].text;
            if (isRedirectInput) redirectedInputFileName = fileName;
            else redirectedOutputFileName = fileName;
            if (redirections)
            {
                redirections->push_back({isRedirectInput, fileName});
            }
        }
        else
        {
            tokens.push_back(brokenInput[i].text);
        }
    }
}


/**
 * @brief Executes inbuilt shell commands such as 'cd', 'exit', and variable assignment.
 *
 * This function takes a vector of tokens representing a command and checks if it is an
 * inbuilt command. It supports changing the current directory ('cd') and setting environment
 * variables (e.g., variable=value).
 *
 * @param tokens A vector of strings representing the tokens of the command.
 * @return Returns 0 if the command is executed successfully, -1 if there is an error,
 *         or 1 if the command is not an inbuilt command. lastStatus holds its exit status.
 */
int executeInbuiltCommands(vector<string> tokens)
{
    if (tokens.empty())
    {
        lastStatus = 2;
        return -1;
    }

    // Check if the command is 'cd'.
    if (tokens[0] == "cd")
    {
        // Verify the correct number of arguments for 'cd'.
        if (tokens.size() != 2)
        {
            cerr << "mish: cd: expected one directory argument" << endl;
            lastStatus = 2;
            return -1;
        }
        else
        {
            // Change the current directory.
            if (chdir(tokens[1].c_str()) != 0)
            {
                reportError("cd: " + tokens[1], errno);
                lastStatus = 1;
                return -1;
            }
        }
        lastStatus = 0;
        return 0;
    }
    else if (tokens[0] == "exit")
    {
        if (tokens.size() != 1)
        {
            cerr << "mish: exit: expected no arguments" << endl;
            lastStatus = 2;
            return -1;
        }
        // Keep the previous command's status when exiting.
        exitRequested = true;
        return 0;
    }
    // Check if the command involves a valid variable=value assignment.
    else if (isAssignment(tokens[0]))
    {
        if (tokens.size() != 1)
        {
            cerr << "mish: environment assignments must be standalone commands" << endl;
            lastStatus = 2;
            return -1;
        }
        // Extract the variable and value from the token.
        size_t loc = tokens[0].find('=');
        string variable = tokens[0].substr(0, loc);
        string value = tokens[0].substr(loc + 1);

        // Set the environment variable using its C-style name and value.
        if (setenv(variable.c_str(), value.c_str(), 1) != 0)
        {
            reportError("setenv: " + variable, errno);
            lastStatus = 1;
            return -1;
        }
        lastStatus = 0;
        return 0;
    }
    // This command is not implemented as a built-in.
    return 1;
}
/**
 * @brief Executes a sequence of commands with potential input/output redirection and pipes.
 *
 * This function takes a vector of commands, each represented by a structure (`commandsToExecute`).
 * It sets up pipes, forks child processes for each command, handles input/output redirection,
 * and executes the commands using execvp.
 *
 * @param commands A vector of `commandsToExecute` structures representing the commands to be executed.
 * @return The final command's exit status, or a nonzero setup error.
 */
int executeCommands(vector<commandsToExecute> commands)
{
    foregroundInterrupted = false;
    // Hold the pipe file descriptors and the PIDs of actual child processes.
    // Keep the original indexing: command i writes to pipes[i + 1].
    vector<vector<int>> pipes(commands.size() + 1, vector<int>(2, -1));
    vector<pid_t> pids(commands.size(), -1);

    // Create pipes only where a | connects two commands.
    for (size_t i = 0; i < commands.size(); i++)
    {
        if (commands[i].isPipeStart && !createPipe(pipes[i + 1]))
        {
            int error = errno;
            closePipes(pipes);
            reportError("pipe", error);
            return 1;
        }
    }

    int status = lastStatus;
    pid_t backgroundGroup = 0;
    // Process each command; a pipeline's children must all start before waiting.
    for (size_t i = 0; i < commands.size(); i++)
    {
        // Only a standalone foreground built-in may change the parent shell.
        bool parentBuiltin = !commands[i].isPipeStart && !commands[i].isPipeEnd &&
                             !commands[i].isBackground &&
                             isInbuiltCommand(commands[i].tokens);
        if (parentBuiltin)
        {
            status = executeParentBuiltin(commands[i]);
            if (exitRequested) break;
            continue;
        }
        if (!commands[i].isPipeEnd) backgroundGroup = 0;

        // Flush the parent's output before forking so it is not printed twice.
        cout.flush();
        cerr.flush();
        // Fork a child process for the command and store its PID.
        pids[i] = fork();
        if (pids[i] == -1)
        {
            int error = errno;
            closePipes(pipes);
            for (pid_t pid : pids) if (pid > 0) kill(pid, SIGKILL);
            for (pid_t pid : pids) if (pid > 0) waitForChild(pid);
            reportError("fork", error);
            return 1;
        }

        if (pids[i] == 0)
        {
            // Child process logic: foreground children receive Ctrl+C normally.
            resetChildSignals();
            if (commands[i].isBackground && setpgid(0, backgroundGroup) == -1)
            {
                reportError("setpgid", errno);
                _exit(1);
            }
            // A background command must not consume the shell's command input.
            if (commands[i].isBackground && !commands[i].isPipeEnd)
            {
                int nullInput = open("/dev/null", O_RDONLY);
                if (nullInput == -1 || !duplicateTo(nullInput, STDIN_FILENO))
                {
                    reportError("background stdin", errno);
                    _exit(1);
                }
                if (nullInput != STDIN_FILENO) close(nullInput);
            }

            // Create C-style arguments for passing to execvp.
            vector<char *> cArgs;
            for (size_t j = 0; j < commands[i].tokens.size(); j++)
            {
                cArgs.push_back(const_cast<char *>(commands[i].tokens[j].c_str()));
            }
            // Add the null pointer required at the end of the argument list.
            cArgs.push_back(nullptr);

            // Redirect stdout if this command writes into a pipe.
            if (commands[i].isPipeStart)
            {
                if (!duplicateTo(pipes[i + 1][1], STDOUT_FILENO))
                {
                    reportError("dup2", errno);
                    _exit(1);
                }
            }
            // Redirect stdin if this command reads from a pipe.
            if (commands[i].isPipeEnd)
            {
                if (!duplicateTo(pipes[i][0], STDIN_FILENO))
                {
                    reportError("dup2", errno);
                    _exit(1);
                }
            }
            // Every original pipe descriptor can now close; dup2 kept
            // the required connections on stdin/stdout.
            closePipes(pipes);

            // File redirections override the corresponding pipe connection.
            if (!applyRedirections(commands[i])) _exit(1);
            if (isInbuiltCommand(commands[i].tokens))
            {
                executeInbuiltCommands(commands[i].tokens);
                _exit(lastStatus);
            }

            // Execute the command using its null-terminated C-string arguments.
            if (execvp(commands[i].tokens[0].c_str(), cArgs.data()) == -1)
            {
                // Save errno immediately; cleanup must not replace the error.
                int error = errno;
                reportError(commands[i].tokens[0], error);
                _exit(error == ENOENT ? 127 : 126);
            }
        }
        else if (commands[i].isBackground)
        {
            if (backgroundGroup == 0) backgroundGroup = pids[i];
            // Both parent and child set the group to cover the fork/exec race.
            if (setpgid(pids[i], backgroundGroup) == -1 &&
                errno != EACCES && errno != ESRCH)
            {
                reportError("setpgid", errno);
            }
            backgroundPids.push_back(pids[i]);
            status = lastStatus = 0;
        }
    }

    // Close all pipe descriptors in the parent process.
    closePipes(pipes);
    // Wait only for foreground children, using real PIDs rather than zero.
    for (size_t i = 0; i < commands.size(); i++)
    {
        if (pids[i] > 0 && !commands[i].isBackground)
        {
            int childStatus = waitForChild(pids[i]);
            if (i + 1 == commands.size()) status = childStatus;
        }
    }
    interrupted = 0;
    drainSignalPipe();
    // The final command supplies the line's exit status.
    return status;
}


/**
 * @brief Opens a script input file descriptor.
 *
 * This function attempts to open a script using the provided file name.
 * If successful, it returns true; otherwise, it reports an error and returns false.
 *
 * @param fin Reference to the script's input file descriptor.
 * @param fileName The name of the file to be opened for input.
 * @return Returns true if the file is opened successfully; otherwise, returns false.
 */
bool openInput(int &fin, string fileName)
{
    // Open the script with close-on-exec so child programs do not inherit it.
    fin = open(fileName.c_str(), O_RDONLY | O_CLOEXEC);
    // Check for success.
    if (fin == -1)
    {
        reportError(fileName, errno);
        return false;
    }
    return true;
}


/**
 * @brief Opens an output file stream.
 *
 * This function attempts to open an output file stream using the provided file name.
 * If successful, it returns true; otherwise, it outputs an error message and returns false.
 *
 * @param fout Reference to an output file stream object.
 * @param fileName The name of the file to be opened for output.
 * @return Returns true if the file is opened successfully; otherwise, returns false.
 */
bool isOutputOpen(ofstream &fout, string fileName)
{
    // opening the file
    fout.open(fileName);
    // check for success
    if (!fout.is_open())
    {
        // output an error message
        cerr << "Unable to open output file: " << fileName << endl;
        // return false if not opened
        return false;
    }

    // opened successfully
    return true;
}

// Helpers for quoting, descriptor handling, and signal-safe input.
namespace {

void reportError(const std::string& context, int error) {
    std::cerr << "mish: " << context << ": " << std::strerror(error) << '\n';
}

void handleSignal(int number) {
    const int savedError = errno;
    if (number == SIGINT) interrupted = 1;
    if (signalWriteFd >= 0) {
        const char event = static_cast<char>(number);
        const ssize_t written = write(signalWriteFd, &event, 1);
        (void)written;
    }
    errno = savedError;
}

bool duplicateTo(int source, int target) {
    int result;
    do {
        result = dup2(source, target);
    } while (result == -1 && errno == EINTR);
    return result != -1;
}

bool createPipe(vector<int>& descriptors, bool nonblocking) {
    if (pipe(descriptors.data()) == -1) return false;
    for (int descriptor : descriptors) {
        if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1 ||
            (nonblocking && fcntl(descriptor, F_SETFL, O_NONBLOCK) == -1)) {
            const int error = errno;
            close(descriptors[0]);
            close(descriptors[1]);
            descriptors = {-1, -1};
            errno = error;
            return false;
        }
    }
    return true;
}

bool initializeSignals() {
    // Internal descriptors must not take the place of closed stdin/stdout/stderr.
    for (int descriptor = 0; descriptor < 3; descriptor++) {
        if (fcntl(descriptor, F_GETFD) == -1 && errno == EBADF) {
            if (open("/dev/null", descriptor == 0 ? O_RDONLY : O_WRONLY) == -1) {
                return false;
            }
        }
    }
    if (!createPipe(signalPipe, true)) {
        reportError("signal pipe", errno);
        return false;
    }
    signalWriteFd = signalPipe[1];
    struct sigaction action {};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGCHLD);
    for (int number : {SIGINT, SIGCHLD}) {
        if (sigaction(number, &action, nullptr) == -1) {
            reportError("sigaction", errno);
            return false;
        }
    }
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGQUIT, &action, nullptr) == -1) {
        reportError("sigaction", errno);
        return false;
    }
    return true;
}

void resetChildSignals() {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (int number : {SIGINT, SIGQUIT, SIGCHLD}) {
        sigaction(number, &action, nullptr);
    }
    close(signalPipe[0]);
    close(signalPipe[1]);
    signalWriteFd = -1;
}

void reapBackgroundChildren() {
    auto& children = backgroundPids;
    for (auto child = children.begin(); child != children.end();) {
        const pid_t result = waitpid(*child, nullptr, WNOHANG);
        if (result > 0 || (result == -1 && errno == ECHILD)) {
            child = children.erase(child);
        } else {
            ++child;
        }
    }
}

void drainSignalPipe() {
    char events[128];
    while (read(signalPipe[0], events, sizeof(events)) > 0) {
    }
    reapBackgroundChildren();
}

bool isAssignment(const std::string& word) {
    const std::size_t equals = word.find('=');
    if (equals == std::string::npos || equals == 0) return false;
    const auto letter = [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') || character == '_';
    };
    if (!letter(static_cast<unsigned char>(word[0]))) return false;
    for (std::size_t i = 1; i < equals; ++i) {
        const unsigned char character = static_cast<unsigned char>(word[i]);
        if (!letter(character) && !(character >= '0' && character <= '9')) return false;
    }
    return true;
}

int waitForChild(pid_t child) {
    int status;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) {
            reportError("waitpid", errno);
            return 1;
        }
        drainSignalPipe();
    }
    foregroundInterrupted =
        WIFSIGNALED(status) && WTERMSIG(status) == SIGINT;
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

ReadResult readLine(int input, std::string& line) {
    line.clear();
    while (true) {
        if (interrupted) {
            interrupted = 0;
            return ReadResult::Interrupted;
        }
        struct pollfd descriptors[2] = {
            {input, POLLIN, 0}, {signalPipe[0], POLLIN, 0}
        };
        if (poll(descriptors, 2, -1) == -1) {
            if (errno == EINTR) continue;
            reportError("reading input", errno);
            return ReadResult::Error;
        }
        if (descriptors[1].revents & POLLIN) {
            drainSignalPipe();
            if (interrupted) continue;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            char character;
            const ssize_t count = read(input, &character, 1);
            if (count == 0) {
                return line.empty() ? ReadResult::End : ReadResult::Line;
            }
            if (count == -1) {
                if (errno == EINTR) continue;
                reportError("reading input", errno);
                return ReadResult::Error;
            }
            if (character == '\n') return ReadResult::Line;
            line += character;
        }
    }
}


void shutdownSignals()
{
    reapBackgroundChildren();
    signalWriteFd = -1;
    close(signalPipe[0]);
    close(signalPipe[1]);
}

bool readShellInput(int input, string& line, bool terminal)
{
    ReadResult result = readLine(input, line);
    if (result == ReadResult::Line) return true;
    if (result == ReadResult::Error) lastStatus = 1;
    if (result == ReadResult::Interrupted)
    {
        lastStatus = 128 + SIGINT;
        if (terminal)
        {
            cout << endl;
            line.clear();
            return true;
        }
    }
    return false;
}

// Track both decoded arguments and their raw positions. The original parser
// can still split command substrings without mistaking quoted | or & for syntax.
bool readTokens(const string& input, vector<shellToken>& tokens, string& error)
{
    string word;
    bool hasWord = false;
    size_t wordStart = 0;
    char quote = '\0';
    auto finishWord = [&](size_t end) {
        if (hasWord)
        {
            tokens.push_back({word, wordStart, end, false});
            word.clear();
            hasWord = false;
        }
    };
    for (size_t i = 0; i < input.size(); i++)
    {
        char character = input[i];
        if (quote == '\'')
        {
            if (character == '\'') quote = '\0';
            else word += character;
        }
        else if (quote == '"')
        {
            if (character == '"') quote = '\0';
            else if (character == '\\' && i + 1 < input.size() &&
                     (input[i + 1] == '"' || input[i + 1] == '\\' ||
                      input[i + 1] == '$' || input[i + 1] == static_cast<char>(96)))
            {
                word += input[++i];
            }
            else word += character;
        }
        else if (character == '\'' || character == '"')
        {
            if (!hasWord) wordStart = i;
            quote = character;
            hasWord = true;
        }
        else if (character == '\\')
        {
            if (!hasWord) wordStart = i;
            if (++i == input.size())
            {
                error = "unfinished escape";
                return false;
            }
            word += input[i];
            hasWord = true;
        }
        else if (isspace(static_cast<unsigned char>(character)))
        {
            finishWord(i);
        }
        else if (character == '|' || character == '&' ||
                 character == '<' || character == '>')
        {
            finishWord(i);
            tokens.push_back({string(1, character), i, i + 1, true});
        }
        else if (character == ';')
        {
            error = "unsupported operator ';'";
            return false;
        }
        else
        {
            if (!hasWord) wordStart = i;
            word += character;
            hasWord = true;
        }
    }
    if (quote != '\0')
    {
        error = "unterminated quote";
        return false;
    }
    finishWord(input.size());
    return true;
}

bool isInbuiltCommand(const vector<string>& tokens)
{
    return !tokens.empty() &&
           (tokens[0] == "cd" || tokens[0] == "exit" || isAssignment(tokens[0]));
}

bool applyRedirections(const commandsToExecute& command)
{
    for (const auto& redirection : command.redirections)
    {
        int flags = redirection.isInput ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
        int descriptor = open(redirection.fileName.c_str(), flags, 0666);
        if (descriptor == -1)
        {
            reportError(redirection.fileName, errno);
            return false;
        }
        int target = redirection.isInput ? STDIN_FILENO : STDOUT_FILENO;
        bool redirected = duplicateTo(descriptor, target);
        int error = errno;
        if (descriptor != target) close(descriptor);
        if (!redirected)
        {
            reportError("dup2", error);
            return false;
        }
    }
    return true;
}

// Apply the same redirections to a parent built-in, then restore its streams.
int executeParentBuiltin(const commandsToExecute& command)
{
    int savedInput = -1;
    int savedOutput = -1;
    cout.flush();
    if (!command.redirections.empty())
    {
        savedInput = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
        savedOutput = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        if (savedInput == -1 || savedOutput == -1)
        {
            int error = errno;
            if (savedInput != -1) close(savedInput);
            if (savedOutput != -1) close(savedOutput);
            reportError("saving standard streams", error);
            return lastStatus = 1;
        }
    }
    if (!applyRedirections(command)) lastStatus = 1;
    else executeInbuiltCommands(command.tokens);
    int status = lastStatus;
    cout.flush();
    if (savedInput != -1)
    {
        if (!duplicateTo(savedInput, STDIN_FILENO) ||
            !duplicateTo(savedOutput, STDOUT_FILENO))
        {
            reportError("restoring standard streams", errno);
            status = 1;
        }
        close(savedInput);
        close(savedOutput);
    }
    return lastStatus = status;
}

void closePipes(const vector<vector<int>>& pipes)
{
    for (const auto& descriptors : pipes)
    {
        for (int descriptor : descriptors)
        {
            if (descriptor != -1) close(descriptor);
        }
    }
}


} // namespace
