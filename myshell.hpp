// myshell.hpp

// ensures header is only included once, prevents compilation issues
#pragma once

#include <string>
#include <vector>

// prints shell prompt
void print_prompt();

// reads line from stdin, true means line was successfully read, false if end of file
bool read_line(std::string& line);

// splits input line by whitespace into tokens
std::vector<std::string> tokenize_whitespace(const std::string& line);


// returns true if command is builtin like cd or exit
bool is_builtin(const std::vector<std::string>& tokens);


// executes built in command inside shell process, 0 for success, -1 if exit, >0 if error
int run_builtin(const std::vector<std::string>& tokens);

// executes non built in command using fork and execvp, shell waits for child to finish
int run_external(const std::vector<std::string>& tokens);

// waits for child
int wait_for_child(int pid);

// builds argv array for execvp, must be null terminated
std::vector<char*> build_argv(const std::vector<std::string>& tokens);
