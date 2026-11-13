# Mines Shell (mish)

A small C++ Unix shell for learning command parsing, processes, pipes, and file
descriptors. Run it on Linux or inside Ubuntu on WSL.

## Build and run

Requires a C++17 compiler. Tests also require Python 3 and common Unix utilities.
On Ubuntu, the development tools can be installed with:

    sudo apt update
    sudo apt install g++ make python3

From this project's directory:

    make
    ./mish

An equivalent direct build command is:

    g++ -std=c++17 -Wall -Wextra -Wpedantic -g Main.cpp -o mish

Run one command per line from a file:

    ./mish commands.mish

Batch mode and piped input print only command output and errors. The welcome
message and working-directory prompt appear when stdin is a terminal.

## Supported commands

mish runs programs from `PATH` and includes `cd <directory>`, `exit`, and
standalone environment assignments.

    echo hello
    cd /tmp
    MISH_TEST="hello world"
    printenv MISH_TEST
    tr a-z A-Z < input.txt > output.txt
    echo hello | tr a-z A-Z | wc -c
    sleep 2 & echo ready
    exit

Use `<` and `>` to redirect files, `|` to connect commands, and `&` to run a
command or pipeline in the background. Quotes, escapes, and tabs work in
arguments. Ctrl+C interrupts a foreground command and returns to the prompt.

Batch scripts stop on syntax errors and continue after command failures.
The shell returns the last command's exit status.

Variable expansion, wildcards, command substitution, multiline quotes,
`VAR=value command`, `>>`, stderr redirection, `;`, `&&`, `||`, and
`jobs`/`fg`/`bg` aren't supported.

## Regression tests
    
    python3 test_mish.py -v