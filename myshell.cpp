// myshell.cpp

#include "myshell.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>


void print_prompt() {
    // print prompt and flush output so it appears immediately
    std::cout << "myshell> " << std::flush;
}

bool read_line(std::string& line) {
    // get line
    return static_cast<bool>(std::getline(std::cin, line));
}

std::vector<std::string> tokenize_whitespace(const std::string& line) {
    // use string stream to split by whitespace
    std::istringstream iss(line);
    std::vector<std::string> tokens;

    std::string token;
    // will return false when no token, so this reads all tokens and adds to vector
    while (iss >> token) {
        tokens.push_back(token);
    }

    return tokens;
}

// see if command is built in
bool is_builtin(const std::vector<std::string>& tokens) {
    // if tokens is empty then return false
    if (tokens.empty()) return false;

    // will be extended later with other commands
    return (tokens[0] == "cd" || tokens[0] == "exit");
}

// ehlper function for cd
static int builtin_cd(const std::vector<std::string>& tokens) {
    const char* target = nullptr;

    // if directory is provided: cd <dir>
    if (tokens.size() >= 2) {
        target = tokens[1].c_str();
    } else {
        // Default: cd -> HOME
        target = std::getenv("HOME");
        if (!target) {
            std::cerr << "cd: HOME not set\n";
            return 1;
        }
    }

    // attempt directory change with chdir syscall
    if (chdir(target) != 0) {
        std::cerr << "cd: " << std::strerror(errno) << "\n";
        return 1;
    }

    return 0;
}

// run the built in command
int run_builtin(const std::vector<std::string>& tokens) {
    // if empty then return
    if (tokens.empty()) return 0;

    // if exit then signal main to terminate loop
    if (tokens[0] == "exit") {
        return -1;
    }

    // cd builtin
    if (tokens[0] == "cd") {
        return builtin_cd(tokens);
    }

    // failsafe incase is_builtin is incorrect
    std::cerr << "Unknown builtin\n";
    return 1;
}

// generate argv for execvp
std::vector<char*> build_argv(const std::vector<std::string>& tokens) {
    // argv needs to be char array with null termination
    std::vector<char*> argv;

    // for strings in tokens add character arrays (strings) to argv
    for (const std::string& s : tokens) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }

    // null-terminate argv
    argv.push_back(nullptr);
    return argv;
}

// waiting for child
int wait_for_child(int pid) {
    int status = 0;

    // use wait pid syscall to wait for child
    while (true) {
        pid_t w = waitpid(pid, &status, 0);

        if (w == -1) {
            if (errno == EINTR) {
                // retry
                continue;
            }
            std::cerr << "waitpid: " << std::strerror(errno) << "\n";
            return 1;
        }
        break;
    }

    // normal exit
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    // sigint
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}

// run external command
int run_external(const std::vector<std::string>& tokens) {
    // if empty return
    if (tokens.empty()) return 0;

    // fork to create a child process
    pid_t pid = fork();

    // if fork fails
    if (pid < 0) {
        std::cerr << "fork: " << std::strerror(errno) << "\n";
        return 1;
    }

    // if we are in the child process
    if (pid == 0) {
        
        std::vector<char *> argv = build_argv(tokens);

        // use execvp to launch command
        execvp(argv[0], argv.data());

        // if execvp returns, an error occurred
        std::cerr << tokens[0] << ": "
                  << std::strerror(errno) << "\n";

        // exit child immediately
        // 127 means command not found
        _exit(127);
    }

    // have the parent process wait for the child
    return wait_for_child(pid);
}
