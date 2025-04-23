#include "mish.h"


/*
    isFile variable is a boolean variable that would have either a true value
    It will be true of we are reading from a file and false otherwise
    this variable is mainly used to handle error. if it is a bad file, just
    exit the program
*/
bool isFile;

/**
 * @brief The main function for Mish Shell.
 *
 * This function serves as the entry point for Mish Shell. The shell can
 * only be run in. Two states, interactive or interactive. It determines
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
    // Check if arguments were passed to the shell to run commands from a file
    if (argc == 1)
    {
        // If no arguments were passed. Running in interactive mode
        cout << "*******************************************" << endl;
        cout << "       WELCOME TO MISH SHELL" << endl;
        cout << "*******************************************" << endl;

        interactive();
    }
    else if (argc == 2)
    {
        // If 1 argument was passed. Running in non-interactive mode with a
        //script. where argv[1] is the the name of the script file
        cout << "**************************************************" << endl;
        cout << "WELCOME TO MISH SHELL. YOUR SCRIPT IS RUNNING" << endl;
        cout << "**************************************************" << endl;
        nonInteractive(argv[1]);
    }
    else if (argc > 2)
    {
        // Invalid number of arguments so print an error and exit
        perror("Invalid arguments");
        exit(0);
    }

    // Exit the program
    return 0;
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
        cout << cwd << " > ";
    }
}


/**
 * @brief Implements an interactive shell.
 *
 * This function creates an interactive shell that continuously reads user input
 * until the user enters "exit". It displays a prompt, processes user input, and
 * repeats the process until the exit command is given. It also updates the isFile
 * variable
 */
void interactive() {
    string input;

    //Not a file if it is an interactive call
    isFile = false;

    // Display initial prompt from the printPrompt() function
    printPrompt();

    // Continue reading user input with getline until the user enters "exit"
    while (getline(cin, input) && input != "exit") {
        // Skip processing if the input is empty
        if (input.empty()) {
            printPrompt();
            continue;
        }


        /* Process the user input
         * This function calls other functions like
         * reduceSpacesAndTrim, generateTokens, and executeCommands
         * to process the input from the user. It handles parallel
         * and pipe commands as well.
         */
        processInput(input);

        // Display prompt for the next input
        printPrompt();
    }
}


/**
 * @brief Processes input from a non-interactive source (file).
 *
 * This function reads input from a specified file, processes each line using
 * the processInput() function, and then exits. It is intended for batch processing
 * of commands stored in a file. It also updates the isFile variable to true
 *
 * @param fileName The name of the input file to be processed.
 */
void nonInteractive(string fileName)
{
    ifstream fin;
    vector<string> tokens;
    string outputFile;
    // Input from file if it is nonInteractive
    isFile = true;

    // open the script file using the is
    if (!openInput(fin, fileName))
    {
        //print error if unable to open input file
        perror("unable to open input file");
        exit(0);
    }

    string input;
    // Read each line from the input file and process using processInput()
    while (getline(fin, input)) {
        if (input.empty()) {
            continue;
        }


        /* Process the user input
         * This function calls other functions like
         * reduceSpacesAndTrim, generateTokens, and executeCommands
         * to process the input from the user. It handles parallel
         * and pipe commands as well.
         */
        processInput(input);
    }
    exit(0);
}
/**
 * @brief Processes an input command.
 *
 * @param input The raw input command to be processed.
 *
 * @details This function takes an input command and processes it to identify
 * individual commands and their parameters. It then executes the commands in
 * parallel or sequentially based on the presence of the '&' or '|' symbols.
 *
 * @note The input command is expected to be a string containing one or more
 *       commands separated by '&' or '|' symbols for parallel or sequential
 *       execution, respectively.
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
 *   and execute them in parallel.
 */

void processInput(string input)
{
    // Initialize variables to keep track of parallel commands, tokens, and file redirections
    // is no parallel commands then basically there is one command so initialize it to 1
    int parallelCommandCount = 1;
    //variable to hold tokens of each valid command
    vector<string> tokens;

    // input and output file name variables
    string redirectedOutputFileName;
    string redirectedInputFileName;

    //the reduceSpacesAndTrim function iterates through each character and
    // corrects the input string if there
    // are any errors
    input = reduceSpacesAndTrim(input);

    //if the input string is empty after reducing spaces and trimming,
    //then ignore it
    if(input.empty())
    {
        return;
    }

    // Iterate through each character in the input string to count the number of parallel commands
    for (size_t i = 0; i < input.size(); i++)
    {
        // Check if the current character is '&' or '|'
        if (input[i] == '&' || input[i] == '|')
        {
            // Increment the parallelCommandCount to track the number of parallel commands
            parallelCommandCount += 1;
        }
    }


    // Create a vector of 'commandsToExecute' objects to represent parallel commands
    // this vecotor is of type commandsToExecute which is a struct
    vector<commandsToExecute> commands(parallelCommandCount);
    // Initialize each 'commandsToExecute' object in the vector with default values

    for (int i = 0; i < parallelCommandCount; i++)
    {
        // Set default values for each member of 'commandsToExecute'
        commands[i].redirectOutputToFile = false;
        commands[i].redirectedInputFromFile = false;
        commands[i].isPipeEnd = false;
        commands[i].isPipeStart = false;
    }
    // Loop through each parallel command to process and configure its properties
    //of each parallel or pipe command
    for (int i = 0; i < parallelCommandCount; i++)
    {
        // Check if it's the last parallel command
        if (i == parallelCommandCount - 1)
        {
            // Check if the last character of the input is alphanumeric,
            // ensuring a valid command
            bool isLastValid = false;
            for (size_t j = 0; j < input.size(); j++)
            {
                if (isalpha(input[j]))
                {
                    isLastValid = true;
                }
            }
            // If the last command is not valid, print an error
            // message and exit if it's a file operation
            if (!isLastValid)
            {
                perror("invalid command \n");
                if (isFile)
                {
                    exit(1);
                }
            }
            // Generate tokens and configure properties for the last parallel commands
            generateTokens(input, tokens, redirectedOutputFileName, redirectedInputFileName);
            //update the commands variable
            commands[i].tokens = tokens;
            // Check for output redirection
            if (input.find('>') != string::npos)
            {
                //update the redirection properties
                //set output redirection to true
                commands[i].redirectOutputToFile = true;
                commands[i].redirectOutputFileName = redirectedOutputFileName;
            }
            // Check for input redirection and update the command
            if (input.find('<') != string::npos)
            {
                //update the redirection properties
                commands[i].redirectedInputFromFile = true;
            }
            //clear the tokens variable
            tokens.clear();
            break;
        }
        //set location of | and & in the command to int max
        size_t loc1 = INT_MAX;
        size_t loc2 = INT_MAX;
        //find the location of | and & and update it
        if (input.find('|') != string::npos)
        {
            loc2 = input.find('|');
        }

        if (input.find('&') != string::npos)
        {
            loc1 = input.find('&');
        }
        // Determine the smallest location of | and & to generate details about
        // the next command
        size_t loc = loc1;
        if (loc1 > loc2)
        {
            loc = loc2;
            //if pipe is the next command to be in the commands vector
            commands[i].isPipeStart = true;
            //update the following command to be end of the pipe
            commands[i + 1].isPipeEnd = true;
        }
        // set temp input to the fist valid command from input
        string tempInput = input.substr(0, loc - 1);
        // update input to be the remaining part of the command
        input = input.substr(loc + 2);
        //generate tokens form tempInput. which basically breaks down each command
        generateTokens(tempInput, tokens, redirectedOutputFileName, redirectedInputFileName);

        //check for input and output redirection and update the properties of the
        //appropriate command
        if (tempInput.find('>') != string::npos)
        {
            commands[i].redirectOutputToFile = true;
        }
        if (tempInput.find('<') != string::npos)
        {
            commands[i].redirectedInputFromFile = true;
        }
        //update tokens of the command vector
        commands[i].tokens = tokens;
        //update recirecting file name
        commands[i].redirectOutputFileName = redirectedOutputFileName;
        tokens.clear();
    }
    // Execute the processed commands

    // The executeCommands function is invoked to execute the commands that have
    // been processed and stored in the 'commands' vector. This function is
    // responsible for interpreting and carrying out the execution of each command.
    // The 'commands' vector serves as the input, providing the set of commands
    // to be executed, which could include both parallel and sequential commands.

    executeCommands(commands);
    //clear the commands
    commands.clear();
}


/**
 * @brief Reduces spaces and trims a given input string.
 *
 * This function fixes the input string by performing the following operations:
 * - Removes trailing spaces from the end of the input string.
 * - Breaks the input string into individual tokens, considering special characters
 *   such as '&', '|', '>', '<' as separate tokens.
 * - Skips consecutive spaces in the input.
 * - Checks for valid input/output redirection and pipe commands.
 * - Handles invalid parallel commands.
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
    // Variables
    string result; // The final processed string
    vector<string> brokenString; // Vector to store individual tokens

    // Remove trailing spaces from the end of the input string
    while (!input.empty() && input.back() == ' ')
    {
        input.pop_back();
    }
    // Process the input string
    string str="";
    for (size_t i = 0; i < input.size(); i++) {
        // Tokenize based on certain characters, if these are encountered split the
        // string and update the token
        if (input[i] == '&' || input[i] == '|' || input[i]=='>' || input[i]=='<') {

            //check of  "| &" in the token, if present=> error
            if (i<input.size()-2)
            {
                if(input[i] == '|' && input[i+1]== ' ' && input[i+2]== '&')
                {
                    perror("Invalid input / output redirecting command \n");
                    if (isFile)
                    {
                        exit(1);
                    }
                    result ="";
                    return result;
                }
            }
            // if not empty then push the brokenString
            if (!str.empty()) {
                brokenString.push_back(str);
                str.clear();
            }
            // Include '&' and '|' as individual tokens
            brokenString.push_back(std::string(1, input[i]));
            //tokenize if there is a space
        } else if (input[i] == ' ') {
            // if not empty then push the brokenString
            if (!str.empty()) {
                brokenString.push_back(str);
                str.clear();
            }
            // Skip consecutive spaces
            while (i + 1 < input.size() && input[i + 1] == ' ') {
                i++;
            }
        }
        //update str with a new character if it was not tokenized
        else
        {
            str = str + input[i];
        }
    }
    // Add the last token if not empty
    if(!str.empty())
    {
        //push
        brokenString.push_back(str);
    }

    // Check for invalid input/output, parallel and pipe commands
    for (size_t i = 0; i < brokenString.size(); i++)
    {
        // if input or out put redirection, amke sure there is a string(file)
        // to perform the operation on
        if (brokenString[i]== ">" || brokenString[i] == "<")
        {
            //if the token is first or last char or the next char is not a alpahbet or number
            //then it is incorrect
            if (i == brokenString.size()-1 || !isalnum(brokenString[i+1][0] ) || i==0)
            {
                perror("Invalid input / output redirecting command \n");
                if (isFile)
                {
                    exit(1);
                }
                result ="";
                return result;
            }
        }

        //if input is pipe, make sure the next its not at the start or end
        if (brokenString[i] == "|")
        {
            // pipe command at the beginning or end
            if (i==brokenString.size()-1 || i==0)
            {
                perror("invalid pipe command \n");
                if (isFile)
                {
                    exit(1);
                }
                result ="";
                return result;
            }

        }
        if (brokenString[i] == "&")
        {
            //take out all the & from the back of the tokens
            while(brokenString.back()=="&")
            {
                brokenString.pop_back();
            }
            //if there is & followed by | then it is illegal
            if(i<brokenString.size()-1)
            {
                //illegal to have & then |
                if(brokenString[i+1]=="|")
                {
                    perror("invalid parallel commands together \n");
                    if (isFile)
                    {
                        exit(1);
                    }
                    result ="";
                    return result;
                }
            }
            //error to have & in th beginning
            if (i==0)
            {
                perror("invalid parallel command \n");
                if (isFile)
                {
                    exit(1);
                }
                result ="";
                return result;
            }

        }


    }

    //reconstruct the corrected sting
    //initialize result
    result="";
    //iterate through the vector to get each token
    for (size_t i = 0; i < brokenString.size(); i++)
    {

        // add it to the string
        result+=brokenString[i];


        if(i<brokenString.size()-1)
        {
            result+=" ";
        }
        if (brokenString[i]== "&" && brokenString[i+1]=="|")
        {
            i++;
        }
    }

    //if the command is exit then exit the code
    if(result=="exit")
    {
        exit(0);
    }

    return result;

}

/**
 * @brief Generates tokens from the input string
 *
 * This function parses the input string and generates tokens based on spaces. It also
 * identifies redirection of input and output if specified in the input string.
 *
 * @param input The input string to be tokenized.
 * @param tokens A reference to a vector of strings to store the generated tokens.
 * @param redirectedOutputFileName A reference to a string to store the filename if output is redirected.
 * @param redirectedInputFileName A reference to a string to store the filename if input is redirected.
 */
void generateTokens(string input, vector<string> &tokens, string &redirectedOutputFileName, string &redirectedInputFileName)
{
    redirectedInputFileName="";
    // Temporary string to store each token
    string str;
    // Flag to indicate if output redirection is encountered
    bool isRedirectOutput = false;
    // Flag to indicate if input redirection is encountered
    bool isRedirectInput = false;
    // Temporary vector to store input redirection tokens
    vector<string> redirectedInput;

    // Iterate through each character in the input string
    for (size_t  i = 0; i < input.size(); i++)
    {
        // Check for space to separate tokens
        if (input[i] == ' ' && i != input.size() - 1)
        {
            // Check if the current token is an output redirection symbol
            if (str == ">")
            {
                isRedirectOutput = true;
            }
            // Check if the current token is an input redirection symbol
            else if (str == "<")
            {
                isRedirectInput = true;
            }
            //check if  it is a redirectedInput
            else if (isRedirectInput)
            {
                tokens.push_back(str);
                isRedirectInput = false;
            }
            // check if it is not redirected output
            else if (!isRedirectOutput)
            {
                tokens.push_back(str);
            }

            //reinitialize str
            str = "";
        }
        else
        {
            //update str
            str += input[i];
        }
    }
    //if not redirected input then push to the token
    if (!isRedirectOutput)
    {
        tokens.push_back(str);
    }

    //update redirectOutputFileName
    else
    {
        redirectedOutputFileName = str;
    }
}


/**
 * @brief Executes inbuilt shell commands such as 'cd' and variable assignment.
 *
 * This function takes a vector of tokens representing a command and checks if it is an
 * inbuilt command. It supports changing the current directory ('cd') and setting environment
 * variables (e.g., variable=value).
 *
 * @param tokens A vector of strings representing the tokens of the command.
 * @return Returns 0 if the command is executed successfully, -1 if there is an error,
 *         or 1 if the command is not an inbuilt command.
 */
int executeInbuiltCommands(vector<string> tokens)
{

    // Check if the command is 'cd'
    if (tokens[0] == "cd")
    {
        // Verify the correct number of arguments for 'cd'
        if (tokens.size() != 2)
        {
            //error
            perror("Invalid argument for cd command\n");
            // Exit if it is a file
            if (isFile)
            {
                exit(1);
            }
            //return -1 if error
            return -1;
        }
        else
        {
            // Change the current directory
            if (chdir(tokens[1].c_str()) != 0)
            {
                perror("Error changing directory:\n");
                // Exit if being executed from a file
                if (isFile)
                {
                    exit(1);
                }
                //return -1 if error
                return -1;
            }
        }
        // exit inbuilt function is already implemented
        return 0;
    }
    // Check if the command involves variable assignment (variable=value)
    else if (tokens[0].find('=') != string::npos)
    {
        // Extract variable and value from the token
        size_t loc = tokens[0].find('=');
        string variable = tokens[0].substr(0, loc);
        string value = tokens[0].substr(loc + 1);

        // Set environment variable using setenve and the variables converted
        // c string
        if (setenv(variable.c_str(), value.c_str(), 1) != 0)
        {
            //error while executing
            perror("Error setting enverionment variable \n");
            if (isFile)
            {
                exit(1);
            }
            //return -1 if error
            return -1;
        }
        // exit inbuilt function is already implemented
        return 0;
    }
    // exit inbuilt function is not implemented
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
 * @return Returns 0 upon successful execution; otherwise, exits the program with appropriate error messages.
 */
int executeCommands(vector<commandsToExecute> commands)
{
    //initialize 2d vector to hold the file deccriptiors
    vector<vector<int>> pipes(commands.size() + 1);
    //vector to keep track of the pids
    vector<int> pids(commands.size());

    // create the pipes for each parallel command using pipe() function
    for (size_t i = 0; i < pipes.size(); i++)
    {
        //initialize and array
        int temp[2];
        //use the pipe function for each parallel command
        if (pipe(temp) == -1)
        {
            // Handle pipe creation error
            perror("error creating a pipe");
            exit(1);
        }
        //push the created FD's to the vector
        pipes[i].push_back(temp[0]);
        pipes[i].push_back(temp[1]);
    }
    // Loop through each command for parallel and pipe, if it is a single
    //then it will just execute it once
    for (size_t  i = 0; i < commands.size(); i++)
    {
        // Check if the command is an inbuilt command (e.g., exit, cd)
        int isInbuilt = executeInbuiltCommands(commands[i].tokens);
        // If it's an inbuilt command continue
        if(isInbuilt==0)
        {
            continue;
        }
        if (isInbuilt == -1)
        {
            // Handle error executing inbuilt command
            perror("error executing inbuilt command");
            // Close all pipes
            for (size_t  j = 0; j < pipes.size(); j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return 0;
        }

        else if (isInbuilt == 1)
        {
            // Fork a child process for the current command and store it in the vector
            pids[i] = fork();

            if (pids[i] == -1)
            {
                // Handle fork error
                perror("error forking");
                exit(1);
            }

            if (pids[i] == 0)
            {
                // Child process logic

                // Create a vector of C-style strings (char*) for passing arguments to execvp
                vector<char *> cArgs;

                //convert to c strings and push

                for (size_t  j = 0; j < commands[i].tokens.size(); j++)
                {
                    // Convert each C++ string token to a C-style string and add it to the vector
                    cArgs.push_back(const_cast<char *>(commands[i].tokens[j].c_str()));
                }
                // Add a null pointer at the end of the vector to indicate the end of the argument list
                cArgs.push_back(nullptr);

                // Close unwanted pipes before executing the command
                for (size_t  j = 0; j < pipes.size(); j++)
                {
                    // Close read end of pipes that are not related to the current command
                    if (i != j)
                    {
                        close(pipes[j][0]);
                    }
                    // Close write end of pipes that are not related to the next command
                    if (i + 1 != j)
                    {
                        close(pipes[j][1]);
                    }
                }

                // Check if the command is the starting point of a pipe
                if (commands[i].isPipeStart)
                {
                    // Redirect standard output (write end of the pipe) to the next command
                    // by duplicates an open file descriptor
                    if (dup2(pipes[i + 1][1], 1) == -1)
                    {
                        //handle error
                        perror("error in FD dup2 \n");
                        exit(1);
                    }
                    // Close the unused read end of the next pipe
                    close(pipes[i + 1][0]);
                }
                // Check if the command is the ending point of a pipe
                if (commands[i].isPipeEnd)
                {
                    // Redirect standard input (read end of the pipe) to the next command
                    // by duplicates an open file descriptor
                    if (dup2(pipes[i][0], 0) == -1)
                    {
                        perror("error in FD dup2 \n");
                        exit(1);
                    }
                }
                // if it is not end of the pipe and close the read end
                else
                {
                    // Close the unused read end of the current pipe
                    close(pipes[i][0]);
                }
                // Check if the command redirects output to a file
                if (commands[i].redirectOutputToFile)
                {
                    /*
                    if (i - 1 >= 0)
                    {
                        if (commands[i].isPipeEnd)
                        {
                            sleep(0);
                        }
                    }*/


                    // Open the specified file for writing or create it if it doesn't exist
                    int outputFile = open(commands[i].redirectOutputFileName.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                    //check File descriptor
                    if (outputFile == -1)
                    {
                        //error in FD
                        perror("Error opening output file ");
                        exit(1);
                    }

                    // Redirect standard output to the file
                    // by duplicates an open file descriptor
                    if (dup2(outputFile, 1) == -1)
                    {
                        //error in dup2
                        perror("Error duplicating file descriptor");
                        exit(1);
                    }

                    //close the redundant FD
                    close(outputFile);
                }
                // Check if it is not the starting point of a pipe
                if (!commands[i].isPipeStart)
                {
                    // Close the write end of the current pipe
                    close(pipes[i + 1][1]);
                }
                // Execute the command using execvp function that takes the tokens
                //converted to cstring
                if (execvp(commands[i].tokens[0].c_str(), cArgs.data()) == -1)
                {
                    // Handle execvp error and close all pipes
                    for (size_t  j = 0; j < pipes.size(); j++)
                    {
                        close(pipes[j][0]);
                        close(pipes[j][1]);
                    }
                    perror("Please check the command");
                    exit(0);
                }
                // Close file descriptors that were just used
                close(pipes[i][0]);
                close(pipes[i + 1][0]);
                for (size_t  j = 0; j < pipes.size(); j++)
                {
                    //close all FD in child process
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                // Exit the child process after executing the command
                return 0;
            }
        }
    }
    // Close all pipes in the parent process at the end
    for (size_t  j = 0; j < pipes.size(); j++)
    {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }
    // Wait for all child processes to complete using waitpid and all
    //pid that were created and stored in pids vector
    for (size_t  i = 0; i < commands.size(); i++)
    {
        waitpid(pids[i], NULL, 0);
    }
    //all childs completed, now exit
    return 0;
}


/**
 * @brief Opens an input file stream.
 *
 * This function attempts to open an input file stream using the provided file name.
 * If successful, it returns true; otherwise, it outputs an error message and exits the program.
 *
 * @param fin Reference to an input file stream object.
 * @param fileName The name of the file to be opened for input.
 * @return Returns true if the file is opened successfully; otherwise, exits the program.
 */
bool openInput(ifstream &fin, string fileName)
{
    // opening the file
    fin.open(fileName);
    // check for success
    if (!fin.is_open())
    {
        // output an error message
        perror("Unable to open input file");
        // return false if not opened
        exit(0);
    }

    // opened successfully
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
        cout << "Unable to open output file: " << fileName << endl;
        // return false if not opened
        return false;
    }

    // opened successfully
    return true;
}