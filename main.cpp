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

        CommandLine cmdline;
        std::string err;
        // parse the command line and if it fails then print error and continue
        if (!parse_command_line(tokens, cmdline, err)) {
            // print syntax error and continue
            std::cerr << "myshell: " << err << "\n";
            continue;
        }

        // if user used operators then take note of it, will handle the execution later on
        if (contains_operators(tokens)) {
            std::cerr << "myshell: operators parsed, execution not implemented yet (next milestones)\n";
            continue;
        }

        // if no operators then run like usual
        run_external(tokens);
    }

    return 0;
}
