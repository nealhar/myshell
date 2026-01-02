// main.cpp

#include "myshell.hpp"

#include <iostream>

int main() {
    // infinite loop
    while (true) {

        // print the shell prompt
        print_prompt();

        // read the input line
        std::string line;
        // if EOF occurs (ctrl d) then break out of shell
        if (!read_line(line)) {
            std::cout << "\n";
            break;
        }

        // tokenize the line and keep operators
        std::vector<std::string> tokens = tokenize_operators(line);

        // if the input is empty then print prompt again
        if (tokens.empty()) {
            continue;
        }

        // if the command is built in then run it in the shell process
        if (is_builtin(tokens)) {
            int rc = run_builtin(tokens);

            // if it exits then return -1 to break out of loop
            if (rc == -1) {
                break;
            }
            continue;
        }

        // parse tokens into a structured representation
        CommandLine cmdline;
        std::string err;

        // parse the command line and if it fails then print error and continue
        if (!parse_command_line(tokens, cmdline, err)) {
            std::cerr << "myshell: " << err << "\n";
            continue;
        }

        // if user used background token, note it but do not execute yet -- will implement later
        if (cmdline.background) {
            std::cerr << "myshell: background execution not implemented yet\n";
            continue;
        }

        // if command line is a pipeline do not execute yet -- will implement later
        if (cmdline.is_pipeline()) {
            std::cerr << "myshell: pipelines not implemented yet\n";
            continue;
        }

        // run command
        if (cmdline.pipeline.empty()) {
            // should not happen if parser is correct, but keep a failsafe
            std::cerr << "myshell: internal error: no command to run\n";
            continue;
        }

        run_command(cmdline.pipeline[0]);
    }

    return 0;
}
