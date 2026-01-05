// myshell.cpp

#include "myshell.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

// needed for open() flags
#include <fcntl.h>

// needed for signals and terminal control
#include <csignal>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// shell process group id
static pid_t g_shell_pgid = -1;

// shell terminal fd (we use stdin as the controlling terminal)
static int g_shell_terminal_fd = STDIN_FILENO;


// initialize shell process group and attach to terminal
void init_shell_job_control() {
    // only attempt job control if stdin is a terminal
    if (!isatty(g_shell_terminal_fd)) {
        return;
    }

    // ignore interactive job control signals in the shell
    // (full signal semantics will be implemented later)
    std::signal(SIGTTOU, SIG_IGN);
    std::signal(SIGTTIN, SIG_IGN);
    std::signal(SIGTSTP, SIG_IGN);
    std::signal(SIGINT,  SIG_IGN);
    std::signal(SIGQUIT, SIG_IGN);

    // put shell in its own process group
    g_shell_pgid = getpid();

    // setpgid(0, 0) makes this process its own process group leader
    if (setpgid(0, 0) < 0) {
        // if this fails, job control may not work, but shell can still run
        std::cerr << "setpgid: " << std::strerror(errno) << "\n";
    }

    // ensure shell owns the terminal
    if (tcsetpgrp(g_shell_terminal_fd, g_shell_pgid) < 0) {
        // if this fails, pipelines can still work but terminal control may be limited
        std::cerr << "tcsetpgrp: " << std::strerror(errno) << "\n";
    }
}

// give terminal to a process group
static void give_terminal_to(pid_t pgid) {
    // only do this for interactive terminals
    if (!isatty(g_shell_terminal_fd)) return;

    // tcsetpgrp makes the given process group the foreground owner of the terminal
    if (tcsetpgrp(g_shell_terminal_fd, pgid) < 0) {
        std::cerr << "tcsetpgrp: " << std::strerror(errno) << "\n";
    }
}

// reclaim terminal back to the shell
static void reclaim_terminal() {
    if (!isatty(g_shell_terminal_fd)) return;
    if (g_shell_pgid < 0) return;

    if (tcsetpgrp(g_shell_terminal_fd, g_shell_pgid) < 0) {
        std::cerr << "tcsetpgrp: " << std::strerror(errno) << "\n";
    }
}


// represents a background job
class Job {
public:
    // numeric job id: 1, 2, 3, ...
    int job_id = 0;

    // process group id for the job
    pid_t pgid = -1;

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

// add a job to the job table and print its launch info
static void add_job(pid_t pgid, const std::vector<int>& pids, const std::string& cmd) {
    Job j;
    j.job_id = g_next_job_id++;
    j.pgid = pgid;
    j.pids = pids;
    j.cmd = cmd;
    j.running = true;

    g_jobs.push_back(j);

    // print job launch line similar to real shells
    // choose the pgid as the representative id for the job
    std::cout << "[" << j.job_id << "] " << j.pgid << "\n";
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

    // builtins are executed in the shell process
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

// reap finished background jobs using WNOHANG
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

int run_command(const Command& cmd, bool background, int* out_pid, int* out_pgid) {
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

    // child process
    if (pid == 0) {
        // put child into its own process group (pgid = child pid)
        // setpgid(0,0) makes this child a process group leader
        if (setpgid(0, 0) < 0) {
            // if this fails, job control may be limited, but try to continue
            std::cerr << "setpgid: " << std::strerror(errno) << "\n";
        }

        // apply <
        if (cmd.has_stdin()) {
            int fd = open(cmd.stdin_file.c_str(), O_RDONLY);
            if (fd < 0) {
                std::cerr << cmd.stdin_file << ": " << std::strerror(errno) << "\n";
                _exit(1);
            }
            if (dup2(fd, STDIN_FILENO) < 0) {
                std::cerr << "dup2: " << std::strerror(errno) << "\n";
                close(fd);
                _exit(1);
            }
            close(fd);
        }

        // apply > or >>
        if (cmd.has_stdout()) {
            int flags = O_WRONLY | O_CREAT;
            if (cmd.append) {
                flags |= O_APPEND;
            } else {
                flags |= O_TRUNC;
            }

            int fd = open(cmd.stdout_file.c_str(), flags, 0644);
            if (fd < 0) {
                std::cerr << cmd.stdout_file << ": " << std::strerror(errno) << "\n";
                _exit(1);
            }
            if (dup2(fd, STDOUT_FILENO) < 0) {
                std::cerr << "dup2: " << std::strerror(errno) << "\n";
                close(fd);
                _exit(1);
            }
            close(fd);
        }

        // build argv and exec
        std::vector<char*> argv = build_exec_argv(cmd);
        execvp(argv[0], argv.data());

        // exec failed
        std::cerr << cmd.argv[0] << ": " << std::strerror(errno) << "\n";
        _exit(127);
    }

    // parent process
    // update 6: put child into its own process group (race-safe: do it in parent too)
    if (setpgid(pid, pid) < 0) {
        // if this fails, job control may be limited, but continue
        // EACCES can happen if child execs very fast; this is often benign
    }

    // update 6: pgid for this job is pid
    pid_t pgid = pid;

    if (out_pid) *out_pid = static_cast<int>(pid);
    if (out_pgid) *out_pgid = static_cast<int>(pgid);

    // background: do not wait, add job and return
    if (background) {
        std::vector<int> pids;
        pids.push_back(static_cast<int>(pid));

        // build display string
        std::string cmd_display;
        for (size_t i = 0; i < cmd.argv.size(); i++) {
            if (i) cmd_display += " ";
            cmd_display += cmd.argv[i];
        }

        add_job(pgid, pids, cmd_display);
        return 0;
    }

    // foreground: give terminal to job, wait, then reclaim terminal
    give_terminal_to(pgid);

    int rc = wait_for_child(pid);

    reclaim_terminal();
    return rc;
}

int run_pipeline(const CommandLine& cmdline, bool background, std::vector<int>* out_pids, int* out_pgid) {
    // if pipeline is empty then return
    if (cmdline.pipeline.empty()) return 0;

    size_t n = cmdline.pipeline.size();

    // single command pipeline just runs command logic
    if (n == 1) {
        int pid = -1;
        int pgid = -1;
        int rc = run_command(cmdline.pipeline[0], background, &pid, &pgid);
        if (out_pids) out_pids->assign(1, pid);
        if (out_pgid) *out_pgid = pgid;
        return rc;
    }

    // validate end-redirection rules:
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

    // store child pids for waiting / job table
    std::vector<int> pids;

    // process group id for the entire pipeline (set after first fork)
    pid_t pgid = -1;

    // previous pipe read end
    int prev_read = -1;

    // create each stage
    for (size_t i = 0; i < n; i++) {
        int pipefd[2] = {-1, -1};

        // last stage does not need a pipe
        if (i < n - 1) {
            if (pipe(pipefd) < 0) {
                std::cerr << "pipe: " << std::strerror(errno) << "\n";
                return 1;
            }
        }

        pid_t pid = fork();

        if (pid < 0) {
            std::cerr << "fork: " << std::strerror(errno) << "\n";
            return 1;
        }

        // child process
        if (pid == 0) {
            // set process group for this stage
            // first stage becomes the group leader
            if (i == 0) {
                // make this child its own process group leader
                if (setpgid(0, 0) < 0) {
                    std::cerr << "setpgid: " << std::strerror(errno) << "\n";
                }
            } else {
                // join existing process group
                if (setpgid(0, pgid) < 0) {
                    std::cerr << "setpgid: " << std::strerror(errno) << "\n";
                }
            }

            // connect stdin from previous pipe
            if (i > 0) {
                if (dup2(prev_read, STDIN_FILENO) < 0) {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    _exit(1);
                }
            }

            // connect stdout to next pipe
            if (i < n - 1) {
                if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    _exit(1);
                }
            }

            // close unused fds
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

            // exec command
            const Command& cmd = cmdline.pipeline[i];
            if (cmd.argv.empty()) {
                std::cerr << "myshell: empty command\n";
                _exit(1);
            }

            std::vector<char*> argv = build_exec_argv(cmd);
            execvp(argv[0], argv.data());

            std::cerr << cmd.argv[0] << ": " << std::strerror(errno) << "\n";
            _exit(127);
        }

        // parent process
        // after first fork, establish pgid as first child's pid
        if (i == 0) {
            pgid = pid;
        }

        // set process group from parent side too (race-safe)
        if (setpgid(pid, pgid) < 0) {
            // benign failures can happen if child execs fast
        }

        pids.push_back(static_cast<int>(pid));

        // parent closes previous read end
        if (prev_read != -1) {
            close(prev_read);
            prev_read = -1;
        }

        // parent keeps read end for next stage
        if (i < n - 1) {
            close(pipefd[1]);
            prev_read = pipefd[0];
        }
    }

    // close any leftover pipe read end
    if (prev_read != -1) {
        close(prev_read);
        prev_read = -1;
    }

    if (out_pids) {
        *out_pids = pids;
    }
    if (out_pgid) {
        *out_pgid = static_cast<int>(pgid);
    }

    // background: do not wait, add job and return
    if (background) {
        // build a display string for pipeline
        std::string cmd_display;
        for (size_t i = 0; i < cmdline.pipeline.size(); i++) {
            if (i) cmd_display += " | ";
            for (size_t k = 0; k < cmdline.pipeline[i].argv.size(); k++) {
                if (k) cmd_display += " ";
                cmd_display += cmdline.pipeline[i].argv[k];
            }
        }

        add_job(pgid, pids, cmd_display);
        return 0;
    }

    // foreground: give terminal to job process group, wait for all, reclaim terminal
    give_terminal_to(pgid);

    int last_status = 0;
    for (size_t i = 0; i < pids.size(); i++) {
        int rc = wait_for_child(pids[i]);
        if (i == pids.size() - 1) {
            last_status = rc;
        }
    }

    reclaim_terminal();
    return last_status;
}


// not used anymore, legacy for simple commands
int run_external(const std::vector<std::string>& tokens) {
    // if empty return
    if (tokens.empty()) return 0;

    pid_t pid = fork();

    if (pid < 0) {
        std::cerr << "fork: " << std::strerror(errno) << "\n";
        return 1;
    }

    if (pid == 0) {
        std::vector<char*> argv = build_argv(tokens);
        execvp(argv[0], argv.data());

        std::cerr << tokens[0] << ": " << std::strerror(errno) << "\n";
        _exit(127);
    }

    return wait_for_child(pid);
}
