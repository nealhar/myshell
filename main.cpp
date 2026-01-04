// main.cpp

#include "myshell.hpp"

#include <iostream>

int main() {
    // infinite loop
    while (true) {

        // update 5: reap finished background processes before showing prompt
        reap_background_jobs();

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
        // builtins run in the shell process because they must modify shell state (like cd)
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

        // if pipeline then run pipeline (foreground or background)
        if (cmdline.is_pipeline()) {

            // background pipeline: do not wait, record job
            if (cmdline.background) {
                std::vector<int> pids;
                run_pipeline(cmdline, true, &pids);
            } else {
                // foreground pipeline: wait like normal
                run_pipeline(cmdline, false, nullptr);
            }

            continue;
        }

        // run single command
        if (cmdline.pipeline.empty()) {
            // should not happen if parser is correct, but keep a failsafe
            std::cerr << "myshell: internal error: no command to run\n";
            continue;
        }

        // background single command: do not wait, record job
        if (cmdline.background) {
            int pid = -1;
            run_command(cmdline.pipeline[0], true, &pid);
        } else {
            // foreground single command
            run_command(cmdline.pipeline[0], false, nullptr);
        }
    }

    return 0;
}
