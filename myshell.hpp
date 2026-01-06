// myshell.hpp

// ensures header is only included once, prevents compilation issues
#pragma once

#include <string>
#include <vector>

// prints shell prompt
void print_prompt();

// reads line from stdin, returns true if line was read, returns false if EOF occurred
// returns true with line="" if interrupted by signal (EINTR) so loop can reap + re-prompt
bool read_line(std::string& line);


// splits input line into tokens while preserving operators
std::vector<std::string> tokenize_operators(const std::string& line);

// initialize shell process group and terminal control
void init_shell_job_control();

// initialize signal handlers (SIGCHLD, SIGINT, SIGTSTP)
void init_shell_signals();

// describes one command in a pipeline
class Command {
    public:
    // argv is the argument vector, argv[0] is the program name
    std::vector<std::string> argv;
    
    // input redirection file (for "<")
    // empty means no redirection specified
    std::string stdin_file;
    
    // output redirection file (for ">" or ">>")
    // empty means no redirection specified
    std::string stdout_file;
    
    // true if output uses append (">>"), false if truncate (">")
    bool append = false;
    
    // helper methods
    bool has_stdin() const;
    bool has_stdout() const;
};

// describes a full command line
class CommandLine {
    public:
    // list of commands separated by pipes
    std::vector<Command> pipeline;

    // true if trailing '&' was present
    bool background = false;
    
    // helper method
    bool is_pipeline() const;
};

/* parses tokens into a structured CommandLine
 returns true if parse succeeded, false if syntax error
 on error, err_msg contains a human readable message*/
bool parse_command_line(const std::vector<std::string>& tokens, CommandLine& out, std::string& err_msg);

// helper to detect if operators are present
bool contains_operators(const std::vector<std::string>& tokens);

// returns true if command is builtin like cd or exit
bool is_builtin(const std::vector<std::string>& tokens);

// executes built in command inside shell process, 0 for success, -1 if exit, >0 if error
int run_builtin(const std::vector<std::string>& tokens);

// executes a single parsed command
// if background == true, shell does not wait
int run_command(const Command& cmd, bool background);

// executes a parsed pipeline
// if background == true, shell does not wait
int run_pipeline(const CommandLine& cmdline, bool background);

// reap any finished/stopped/continued children
void reap_children();

//////////////////////////////////////////////////////
// legacy methods, used in older versions, not anymore
// splits input line by whitespace into tokens -- not used anymore
std::vector<std::string> tokenize_whitespace(const std::string& line);

// legacy -- does not support redirection
int run_external(const std::vector<std::string>& tokens);

// legacy -- builds argv array for execvp, must be null terminated
std::vector<char*> build_argv(const std::vector<std::string>& tokens);