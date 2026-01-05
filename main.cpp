// main.cpp

#include "myshell.hpp"

#include <iostream>

int main() {
    // initialize shell process group and terminal control
    init_shell_job_control();

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

        // pipeline execution path
        if (cmdline.is_pipeline()) {
            if (cmdline.background) {
                // background pipeline: do not wait
                std::vector<int> pids;
                int pgid = -1;
                run_pipeline(cmdline, true, &pids, &pgid);
            } else {
                // foreground pipeline: shell gives terminal to job then waits
                run_pipeline(cmdline, false, nullptr, nullptr);
            }
            continue;
        }

        // single command execution path
        if (cmdline.pipeline.empty()) {
            // should not happen if parser is correct, but keep a failsafe
            std::cerr << "myshell: internal error: no command to run\n";
            continue;
        }

        if (cmdline.background) {
            // background single command: do not wait
            int pid = -1;
            int pgid = -1;
            run_command(cmdline.pipeline[0], true, &pid, &pgid);
        } else {
            // foreground single command
            run_command(cmdline.pipeline[0], false, nullptr, nullptr);
        }
    }

    return 0;
}
