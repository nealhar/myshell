// main.cpp

#include "myshell.hpp"

#include <iostream>

int main() {
    // Infinite loop
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

        // tokenize input
        std::vector<std::string> tokens = tokenize_whitespace(line);

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
        } else {
            // non built in commands run in a child process
            run_external(tokens);
        }
    }

    return 0;
}
