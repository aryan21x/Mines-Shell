#ifndef MISH_HEADERS_H
#define MISH_HEADERS_H

#include <iostream>
#include <string>
#include <cstring>
#include<unistd.h>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <climits>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
using namespace std;



// adding ordered redirects and background state.
struct fileRedirection {
    bool isInput;
    string fileName;
};

struct commandsToExecute {
    vector<string> tokens;
    bool redirectOutputToFile = false;
    bool redirectedInputFromFile = false;
    bool isPipeStart = false;
    bool isPipeEnd = false;
    string redirectOutputFileName;
    string redirectedInputFileName;
    bool isBackground = false;
    vector<fileRedirection> redirections;
};
void interactive();
string reduceSpacesAndTrim(string input);
void nonInteractive(string fileName);
void generateTokens(string input, vector<string> & tokens, string &redirectedFileName , string & redirectedInputFileName,
                    vector<fileRedirection>* redirections = nullptr);
int executeCommands(vector<commandsToExecute> commands);
//int executeCommand(vector<string> tokens, bool outputToFile, string fileName);
bool openInput(int& fin, string fileName);
bool isOutputOpen(ofstream& fout, string fileName);
void processInput(string input);
int executeInbuiltCommands(vector<string> tokens);
#endif
