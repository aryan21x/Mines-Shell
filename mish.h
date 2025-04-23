#ifndef MISH_HEADERS_H
#define MISH_HEADERS_H

#include <iostream>
#include <string>
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



struct commandsToExecute {
    vector<string> tokens;
    bool redirectOutputToFile;
    bool redirectedInputFromFile;
    bool isPipeStart;
    bool isPipeEnd;
    string redirectOutputFileName;
    string redirectedInputFileName;
};
void interactive();
string reduceSpacesAndTrim(string input);
void nonInteractive(string fileName);
void generateTokens(string input, vector<string> & tokens, string &redirectedFileName , string & redirectedInputFileName);
int executeCommands(vector<commandsToExecute> commands);
//int executeCommand(vector<string> tokens, bool outputToFile, string fileName);
bool isInputOpen(ifstream& fin, string fileName);
bool isOutputOpen(ofstream& fout, string fileName);
void processInput(string input);
int executeInbuiltCommands(vector<string> tokens);
#endif