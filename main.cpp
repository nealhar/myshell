// main.cpp

#include "myshell.hpp"

#include <iostream>

int main()
{
    // initialize shell process group and terminal control
    init_shell_job_control();

    // install signal handlers for interactive job control
    init_shell_signals();

    // infinite loop
    while (true)
    {

        // reap any child status changes before showing prompt
        reap_children();

        // print the shell prompt
        print_prompt();

        // read the input line
        std::string line;

        if (!read_line(line))
        {
            std::cout << "\n";
            break;
        }

        // if interrupted by signal, line may be empty; just continue after reaping on next loop
        if (line.empty())
        {
            continue;
        }

        // reap after returning from input as well (covers signal timing)
        reap_children();

        // tokenize the line and keep operators
        std::vector<std::string> tokens = tokenize_operators(line);

        // if the input is empty then print prompt again
        if (tokens.empty())
        {
            continue;
        }

        // if the command is built in then run it in the shell process
        // builtins run in the shell process because they must modify shell state (like cd)
        if (is_builtin(tokens))
        {
            int rc = run_builtin(tokens);

            // if it exits then return -1 to break out of loop
            if (rc == -1)
            {
                break;
            }
            continue;
        }

        // parse tokens into a structured representation
        CommandLine cmdline;
        std::string err;

        // parse the command line and if it fails then print error and continue
        if (!parse_command_line(tokens, cmdline, err))
        {
            std::cerr << "myshell: " << err << "\n";
            continue;
        }

        // run pipeline or single command
        if (cmdline.is_pipeline())
        {
            run_pipeline(cmdline, cmdline.background);
        }
        else
        {
            if (cmdline.pipeline.empty())
            {
                std::cerr << "myshell: internal error: no command to run\n";
                continue;
            }
            run_command(cmdline.pipeline[0], cmdline.background);
        }
    }

    return 0;
}
