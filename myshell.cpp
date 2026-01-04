// myshell.cpp

#include "myshell.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

// needed for open() flags
#include <fcntl.h>

#include <sys/wait.h>
#include <unistd.h>


// ============================================================
// update 5: job table
// ============================================================

// represents a background job
class Job {
public:
    // numeric job id: 1, 2, 3, ...
    int job_id = 0;

    // list of process ids belonging to this job (pipelines have multiple)
    std::vector<int> pids;

    // the command line text (for display)
    std::string cmd;

    // true if job still running, false if fully finished
    bool running = true;

    // helper: check if a pid is part of this job
    bool contains_pid(int pid) const {
        for (int p : pids) {
            if (p == pid) return true;
        }
        return false;
    }

    // helper: remove a pid when it has terminated
    void remove_pid(int pid) {
        for (size_t i = 0; i < pids.size(); i++) {
            if (pids[i] == pid) {
                pids.erase(pids.begin() + i);
                break;
            }
        }
        if (pids.empty()) {
            running = false;
        }
    }
};

// global job table for this simple shell milestone
static std::vector<Job> g_jobs;

// next job id counter
static int g_next_job_id = 1;

// helper to build a display string for tokens (approx original command)
static std::string join_tokens_for_display(const std::vector<std::string>& tokens) {
    std::string s;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (i) s += " ";
        s += tokens[i];
    }
    return s;
}

// add a job to the job table and print its launch info
static void add_job(const std::vector<int>& pids, const std::string& cmd) {
    Job j;
    j.job_id = g_next_job_id++;
    j.pids = pids;
    j.cmd = cmd;
    j.running = true;

    g_jobs.push_back(j);

    // print job launch line similar to real shells
    // choose the first pid as a representative id
    int rep = (pids.empty() ? -1 : pids[0]);
    std::cout << "[" << j.job_id << "] " << rep << "\n";
}

// print jobs builtin output
static void print_jobs() {
    // print each job state
    for (const Job& j : g_jobs) {
        if (j.running) {
            std::cout << "[" << j.job_id << "] Running  " << j.cmd << "\n";
        } else {
            std::cout << "[" << j.job_id << "] Done     " << j.cmd << "\n";
        }
    }
}

// ============================================================
// basic shell utilities
// ============================================================

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

// check for operator
static bool is_operator_char(char c) {
    // operators are single characters except for >>
    return (c == '|' || c == '<' || c == '>' || c == '&');
}

// tokenize the operators
std::vector<std::string> tokenize_operators(const std::string& line) {
    std::vector<std::string> tokens;

    // current token being built
    std::string cur;

    // helper lambda to push the current token to vector if non-empty
    auto flush_cur = [&]() {
        if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    };

    // scan line character by character
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];

        // treat whitespace, tabs, and new lines as delimiters
        if (c == ' ' || c == '\t' || c == '\n') {
            flush_cur();
            continue;
        }

        // handle operators
        if (is_operator_char(c)) {
            flush_cur();

            // special case: >> is one token
            if (c == '>' && (i + 1) < line.size() && line[i + 1] == '>') {
                tokens.push_back(">>");
                i++;
            } else {
                // single char operator
                tokens.push_back(std::string(1, c));
            }
            continue;
        }

        // normal character goes into current token
        cur.push_back(c);
    }

    // flush any remaining token at end of line
    flush_cur();

    return tokens;
}

bool contains_operators(const std::vector<std::string>& tokens) {
    // if any operator tokens exist, return true
    for (const std::string& t : tokens) {
        if (t == "|" || t == "<" || t == ">" || t == ">>" || t == "&") {
            return true;
        }
    }
    return false;
}

bool Command::has_stdin() const {
    // true if input redirection file was provided
    return !stdin_file.empty();
}

bool Command::has_stdout() const {
    // true if output redirection file was provided
    return !stdout_file.empty();
}

bool CommandLine::is_pipeline() const {
    // true if more than one command exists
    return pipeline.size() > 1;
}

// return true if it is a redirect token
static bool is_redirect_token(const std::string& t) {
    return (t == "<" || t == ">" || t == ">>");
}

// return true if it is an operator token
static bool is_operator_token(const std::string& t) {
    return (t == "|" || t == "<" || t == ">" || t == ">>" || t == "&");
}

// parse the command line
bool parse_command_line(const std::vector<std::string>& tokens, CommandLine& out, std::string& err_msg) {
    // reset output
    out = CommandLine{};
    err_msg.clear();

    // if command is empty then return false
    if (tokens.empty()) {
        err_msg = "empty command";
        return false;
    }

    // if operator is misplaced then return false
    if (tokens[0] == "|") {
        err_msg = "syntax error near unexpected token '|'";
        return false;
    }

    // if last token is & then mark it as background and remove it for parsing
    size_t end = tokens.size();
    if (tokens[end - 1] == "&") {
        out.background = true;
        end--;

        // "&" by itself is invalid
        if (end == 0) {
            err_msg = "syntax error near unexpected token '&'";
            return false;
        }
    }

    // current command being built
    Command current;

    // helper to finalize current command into pipeline
    auto flush_command = [&]() -> bool {
        // command must have argv[0]
        if (current.argv.empty()) {
            err_msg = "syntax error: missing command";
            return false;
        }
        out.pipeline.push_back(current);
        current = Command{};
        return true;
    };

    // parse each token
    for (size_t i = 0; i < end; i++) {
        const std::string& t = tokens[i];

        // handle pipeline separator
        if (t == "|") {
            // cannot pipe without a command before it
            if (!flush_command()) return false;

            // next token cannot be end or another pipe
            if (i + 1 >= end || tokens[i + 1] == "|") {
                err_msg = "syntax error near unexpected token '|'";
                return false;
            }
            continue;
        }

        // handle redirections
        if (is_redirect_token(t)) {
            // must have a filename following
            if (i + 1 >= end) {
                err_msg = "syntax error: redirection missing filename";
                return false;
            }
            // filename is next token
            const std::string& file = tokens[i + 1];

            // filename cannot be another operator
            if (is_operator_token(file)) {
                err_msg = "syntax error: redirection missing filename";
                return false;
            }
            // handle what the file is based on preceding operator
            if (t == "<") {
                current.stdin_file = file;
            } else if (t == ">") {
                current.stdout_file = file;
                current.append = false;
            } else if (t == ">>") {
                current.stdout_file = file;
                current.append = true;
            }
            // skip filename token
            i++;
            continue;
        }

        // normal argument token
        current.argv.push_back(t);
    }

    // push last command
    if (!flush_command()) return false;

    return true;
}

// see if command is built in
bool is_builtin(const std::vector<std::string>& tokens) {
    // if tokens is empty then return false
    if (tokens.empty()) return false;

    // update 5: add "jobs" builtin to display background jobs
    return (tokens[0] == "cd" || tokens[0] == "exit" || tokens[0] == "jobs");
}

// helper function for cd
static int builtin_cd(const std::vector<std::string>& tokens) {
    const char* target = nullptr;

    // if directory is provided: cd <dir>
    if (tokens.size() >= 2) {
        target = tokens[1].c_str();
    } else {
        // default cd is home
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

    // jobs builtin
    if (tokens[0] == "jobs") {
        print_jobs();
        return 0;
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

// waiting for child (blocking)
int wait_for_child(int pid) {
    int status = 0;

    // use waitpid syscall to wait for child
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

    // signaled exit
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}

// update 5: reap finished background jobs using WNOHANG
void reap_background_jobs() {
    int status = 0;

    // loop reaping until no more children have exited
    while (true) {
        pid_t pid = waitpid(-1, &status, WNOHANG);

        // no more finished children
        if (pid == 0) {
            break;
        }

        // error: no children or other issue
        if (pid < 0) {
            // if no child processes exist, this is normal in many shells
            if (errno == ECHILD) {
                break;
            }
            // other errors should be reported
            std::cerr << "waitpid: " << std::strerror(errno) << "\n";
            break;
        }

        // find which job this pid belongs to and mark it finished
        for (Job& j : g_jobs) {
            if (j.running && j.contains_pid(static_cast<int>(pid))) {
                j.remove_pid(static_cast<int>(pid));

                // if this was the last pid in the job, job is done
                if (!j.running) {
                    std::cout << "[" << j.job_id << "] Done     " << j.cmd << "\n";
                }
                break;
            }
        }
    }

    // optional cleanup: remove jobs that are done to keep list small
    // keep it simple: leave them so "jobs" shows Done entries too
}

// helper: build argv for execvp from Command.argv
static std::vector<char*> build_exec_argv(const Command& cmd) {
    std::vector<char*> argv;
    for (const std::string& s : cmd.argv) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

// run a single command with redirection
int run_command(const Command& cmd, bool background, int* out_pid) {
    // if no program name exists then return error
    if (cmd.argv.empty()) {
        std::cerr << "myshell: empty command\n";
        return 1;
    }

    // fork to create a child process
    pid_t pid = fork();

    // if fork fails
    if (pid < 0) {
        std::cerr << "fork: " << std::strerror(errno) << "\n";
        return 1;
    }

    // if we are in the child process
    if (pid == 0) {
        // apply <
        if (cmd.has_stdin()) {
            // open input file as read-only
            int fd = open(cmd.stdin_file.c_str(), O_RDONLY);

            // if open fails, print error and exit child
            if (fd < 0) {
                std::cerr << cmd.stdin_file << ": " << std::strerror(errno) << "\n";
                _exit(1);
            }

            // duplicate fd onto stdin (fd 0)
            if (dup2(fd, STDIN_FILENO) < 0) {
                std::cerr << "dup2: " << std::strerror(errno) << "\n";
                close(fd);
                _exit(1);
            }

            // close original fd after dup2
            close(fd);
        }

        // apply > or >>
        if (cmd.has_stdout()) {
            // choose flags based on append vs truncate
            int flags = O_WRONLY | O_CREAT;

            // >> means append, > means truncate
            if (cmd.append) {
                flags |= O_APPEND;
            } else {
                flags |= O_TRUNC;
            }

            // open output file with mode 0644 (rw-r--r--)
            int fd = open(cmd.stdout_file.c_str(), flags, 0644);

            // if open fails, print error and exit child
            if (fd < 0) {
                std::cerr << cmd.stdout_file << ": " << std::strerror(errno) << "\n";
                _exit(1);
            }

            // duplicate fd onto stdout (fd 1)
            if (dup2(fd, STDOUT_FILENO) < 0) {
                std::cerr << "dup2: " << std::strerror(errno) << "\n";
                close(fd);
                _exit(1);
            }

            // close original fd after dup2
            close(fd);
        }

        // build argv for execvp from cmd.argv
        std::vector<char*> argv = build_exec_argv(cmd);

        // run program
        execvp(argv[0], argv.data());

        // if execvp returns, an error occurred
        std::cerr << cmd.argv[0] << ": " << std::strerror(errno) << "\n";

        // exit child immediately
        _exit(127);
    }

    // parent process
    if (out_pid) {
        *out_pid = static_cast<int>(pid);
    }

    // if background, do not wait
    if (background) {
        // add to job table
        std::vector<int> pids;
        pids.push_back(static_cast<int>(pid));

        // command display string: join argv
        std::string cmd_display;
        for (size_t i = 0; i < cmd.argv.size(); i++) {
            if (i) cmd_display += " ";
            cmd_display += cmd.argv[i];
        }

        add_job(pids, cmd_display);
        return 0;
    }

    // foreground waits for child
    return wait_for_child(pid);
}

// update 5: run pipeline, optionally background
int run_pipeline(const CommandLine& cmdline, bool background, std::vector<int>* out_pids) {
    // if pipeline is empty then return
    if (cmdline.pipeline.empty()) return 0;

    // number of commands in the pipeline
    size_t n = cmdline.pipeline.size();

    // validate end-redirection rules (same as update 4):
    // - only first command can have stdin redirection
    // - only last command can have stdout redirection
    for (size_t i = 0; i < n; i++) {
        if (i != 0 && cmdline.pipeline[i].has_stdin()) {
            std::cerr << "myshell: input redirection only allowed on first pipeline stage\n";
            return 1;
        }
        if (i != n - 1 && cmdline.pipeline[i].has_stdout()) {
            std::cerr << "myshell: output redirection only allowed on last pipeline stage\n";
            return 1;
        }
    }

    // store all child pids so parent can wait for all of them
    std::vector<int> pids;

    // previous pipe read end (for stdin of current command)
    int prev_read = -1;

    // create each stage of the pipeline
    for (size_t i = 0; i < n; i++) {

        // create a new pipe for this stage to the next stage
        // last stage does not need a pipe
        int pipefd[2] = {-1, -1};
        if (i < n - 1) {
            if (pipe(pipefd) < 0) {
                std::cerr << "pipe: " << std::strerror(errno) << "\n";
                return 1;
            }
        }

        // fork a child for this stage
        pid_t pid = fork();

        // fork failed
        if (pid < 0) {
            std::cerr << "fork: " << std::strerror(errno) << "\n";
            return 1;
        }

        // child process
        if (pid == 0) {

            // if this is not the first stage, connect stdin to prev_read
            if (i > 0) {
                if (dup2(prev_read, STDIN_FILENO) < 0) {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    _exit(1);
                }
            }

            // if this is not the last stage, connect stdout to pipe write end
            if (i < n - 1) {
                if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    _exit(1);
                }
            }

            // close fds that are no longer needed after dup2
            if (prev_read != -1) close(prev_read);
            if (pipefd[0] != -1) close(pipefd[0]);
            if (pipefd[1] != -1) close(pipefd[1]);

            // apply input redirection on first stage
            if (i == 0 && cmdline.pipeline[i].has_stdin()) {
                int fd = open(cmdline.pipeline[i].stdin_file.c_str(), O_RDONLY);
                if (fd < 0) {
                    std::cerr << cmdline.pipeline[i].stdin_file << ": " << std::strerror(errno) << "\n";
                    _exit(1);
                }
                if (dup2(fd, STDIN_FILENO) < 0) {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    close(fd);
                    _exit(1);
                }
                close(fd);
            }

            // apply output redirection on last stage
            if (i == n - 1 && cmdline.pipeline[i].has_stdout()) {
                int flags = O_WRONLY | O_CREAT;
                if (cmdline.pipeline[i].append) {
                    flags |= O_APPEND;
                } else {
                    flags |= O_TRUNC;
                }

                int fd = open(cmdline.pipeline[i].stdout_file.c_str(), flags, 0644);
                if (fd < 0) {
                    std::cerr << cmdline.pipeline[i].stdout_file << ": " << std::strerror(errno) << "\n";
                    _exit(1);
                }
                if (dup2(fd, STDOUT_FILENO) < 0) {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    close(fd);
                    _exit(1);
                }
                close(fd);
            }

            // build argv and exec the command
            const Command& cmd = cmdline.pipeline[i];
            if (cmd.argv.empty()) {
                std::cerr << "myshell: empty command\n";
                _exit(1);
            }

            std::vector<char*> argv = build_exec_argv(cmd);

            // exec the program
            execvp(argv[0], argv.data());

            // if exec returns, print error
            std::cerr << cmd.argv[0] << ": " << std::strerror(errno) << "\n";
            _exit(127);
        }

        // parent process
        pids.push_back(static_cast<int>(pid));

        // parent no longer needs prev_read (it was used for this stage)
        if (prev_read != -1) {
            close(prev_read);
            prev_read = -1;
        }

        // parent keeps read end of current pipe for next stage
        // parent closes write end immediately
        if (i < n - 1) {
            close(pipefd[1]);
            prev_read = pipefd[0];
        }
    }

    // parent: after spawning all children, close any remaining pipe read end
    if (prev_read != -1) {
        close(prev_read);
        prev_read = -1;
    }

    // if caller wants pids, return them
    if (out_pids) {
        *out_pids = pids;
    }

    // background: do not wait, add as one job
    if (background) {
        // build a display string for the pipeline command
        std::string cmd_display;
        for (size_t i = 0; i < cmdline.pipeline.size(); i++) {
            if (i) {
                cmd_display += " | ";
            }
            for (size_t k = 0; k < cmdline.pipeline[i].argv.size(); k++) {
                if (k) {
                    cmd_display += " ";
                }
                cmd_display += cmdline.pipeline[i].argv[k];
            }
        }

        add_job(pids, cmd_display);
        return 0;
    }

    // foreground: wait for all children, return status of last stage
    int last_status = 0;
    for (size_t i = 0; i < pids.size(); i++) {
        int rc = wait_for_child(pids[i]);

        // store status of the last command in the pipeline
        if (i == pids.size() - 1) {
            last_status = rc;
        }
    }

    return last_status;
}

// not used anymore, legacy for simple commands
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

        std::vector<char*> argv = build_argv(tokens);

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
