// myshell.cpp

#include "myshell.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

// needed for open() flags
#include <fcntl.h>

// signal + terminal control helpers
#include <csignal>
#include <signal.h>
#include <sys/select.h>


#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// shell process group id
static pid_t g_shell_pgid = -1;

// shell terminal fd (we use stdin as the controlling terminal)
static int g_shell_terminal_fd = STDIN_FILENO;

// set by SIGCHLD handler to tell main loop there is work to reap
static volatile sig_atomic_t g_sigchld_flag = 0;

// current foreground process group id (0 or -1 means no foreground job)
static volatile sig_atomic_t g_fg_pgid = 0;


static void give_terminal_to(pid_t pgid) {
    // only do this for interactive terminals
    if (!isatty(g_shell_terminal_fd)) return;

    // tcsetpgrp makes the given process group the foreground owner of the terminal
    if (tcsetpgrp(g_shell_terminal_fd, pgid) < 0) {
        std::cerr << "tcsetpgrp: " << std::strerror(errno) << "\n";
    }
}

static void reclaim_terminal() {
    if (!isatty(g_shell_terminal_fd)) return;
    if (g_shell_pgid < 0) return;

    if (tcsetpgrp(g_shell_terminal_fd, g_shell_pgid) < 0) {
        std::cerr << "tcsetpgrp: " << std::strerror(errno) << "\n";
    }
}


// initialize shell process group and attach to terminal
void init_shell_job_control() {
    // only attempt job control if stdin is a terminal
    if (!isatty(g_shell_terminal_fd)) {
        return;
    }

    // put shell in its own process group
    g_shell_pgid = getpid();

    // setpgid(0,0) makes this process its own process group leader
    if (setpgid(0, 0) < 0) {
        std::cerr << "setpgid: " << std::strerror(errno) << "\n";
    }

    // ensure shell owns the terminal
    if (tcsetpgrp(g_shell_terminal_fd, g_shell_pgid) < 0) {
        std::cerr << "tcsetpgrp: " << std::strerror(errno) << "\n";
    }

    // ignore SIGTTOU so tcsetpgrp does not stop the shell
    std::signal(SIGTTOU, SIG_IGN);
    std::signal(SIGTTIN, SIG_IGN);
}


enum class JobState {
    RUNNING,
    STOPPED,
    DONE
};

// represents a job (background, or stopped foreground that becomes manageable)
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

    // job state
    JobState state = JobState::RUNNING;

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
            state = JobState::DONE;
        }
    }
};

// global job table
static std::vector<Job> g_jobs;

// next job id counter
static int g_next_job_id = 1;

// add job to the job table
static int add_job(pid_t pgid, const std::vector<int>& pids, const std::string& cmd, JobState st) {
    Job j;
    j.job_id = g_next_job_id++;
    j.pgid = pgid;
    j.pids = pids;
    j.cmd = cmd;
    j.state = st;

    g_jobs.push_back(j);
    return j.job_id;
}

// print a job launch line
static void print_job_started(int job_id, pid_t pgid) {
    std::cout << "[" << job_id << "] " << pgid << "\n";
}

// print jobs builtin output
static void print_jobs() {
    for (const Job& j : g_jobs) {
        if (j.state == JobState::RUNNING) {
            std::cout << "[" << j.job_id << "] Running  " << j.cmd << "\n";
        } else if (j.state == JobState::STOPPED) {
            std::cout << "[" << j.job_id << "] Stopped  " << j.cmd << "\n";
        } else {
            std::cout << "[" << j.job_id << "] Done     " << j.cmd << "\n";
        }
    }
}


// SIGCHLD handler: just set a flag
static void sigchld_handler(int) {
    g_sigchld_flag = 1;
}

// SIGINT/SIGTSTP handler: forward to foreground process group if one exists
static void forward_to_foreground(int sig) {
    pid_t pgid = (pid_t)g_fg_pgid;

    // if no foreground job, do nothing
    if (pgid <= 0) {
        return;
    }

    // send signal to the entire foreground process group
    // negative pid means "process group"
    kill(-pgid, sig);
}

static void sigint_handler(int sig) {
    forward_to_foreground(sig);
}

static void sigtstp_handler(int sig) {
    forward_to_foreground(sig);
}

// install signal handlers using sigaction so we can control SA_RESTART behavior
void init_shell_signals() {
    // SIGCHLD: notify main loop to reap children
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));

        sa.sa_handler = sigchld_handler;

        // do NOT set SA_RESTART:
        // we want blocking input to return with EINTR so we can reap and print "Done"
        sa.sa_flags = 0;

        sigemptyset(&sa.sa_mask);

        if (sigaction(SIGCHLD, &sa, nullptr) < 0) {
            std::cerr << "sigaction(SIGCHLD): " << std::strerror(errno) << "\n";
        }
    }

    // SIGINT: forward Ctrl-C to foreground job (shell stays alive)
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));

        sa.sa_handler = sigint_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);

        if (sigaction(SIGINT, &sa, nullptr) < 0) {
            std::cerr << "sigaction(SIGINT): " << std::strerror(errno) << "\n";
        }
    }

    // SIGTSTP: forward Ctrl-Z to foreground job
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));

        sa.sa_handler = sigtstp_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);

        if (sigaction(SIGTSTP, &sa, nullptr) < 0) {
            std::cerr << "sigaction(SIGTSTP): " << std::strerror(errno) << "\n";
        }
    }

    // ignore SIGQUIT like most shells
    std::signal(SIGQUIT, SIG_IGN);
}


void print_prompt() {
    // print prompt and flush output so it appears immediately
    std::cout << "myshell> " << std::flush;
}

bool read_line(std::string& line) {
    // clear output line
    line.clear();

    // read input one byte at a time until newline or EOF
    while (true) {
        char c = 0;

        // read 1 byte from stdin
        ssize_t n = read(STDIN_FILENO, &c, 1);

        // EOF: user pressed Ctrl-D on an empty line
        if (n == 0) {
            // if we already collected characters, treat it as a final line
            // (bash typically executes the line; this behavior is acceptable)
            if (!line.empty()) {
                return true;
            }
            return false;
        }

        // read error
        if (n < 0) {
            // interrupted by signal: return to main loop so it can reap children
            if (errno == EINTR) {
                // leave line as-is (usually empty), caller can reap + re-prompt
                return true;
            }

            // other errors
            std::cerr << "read: " << std::strerror(errno) << "\n";
            return true;
        }

        // newline ends the line
        if (c == '\n') {
            return true;
        }

        // normal character: append
        line.push_back(c);
    }
}


std::vector<std::string> tokenize_whitespace(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;

    std::string token;
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
    for (const std::string& t : tokens) {
        if (t == "|" || t == "<" || t == ">" || t == ">>" || t == "&") {
            return true;
        }
    }
    return false;
}

bool Command::has_stdin() const {
    return !stdin_file.empty();
}

bool Command::has_stdout() const {
    return !stdout_file.empty();
}

bool CommandLine::is_pipeline() const {
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
    out = CommandLine{};
    err_msg.clear();

    if (tokens.empty()) {
        err_msg = "empty command";
        return false;
    }

    if (tokens[0] == "|") {
        err_msg = "syntax error near unexpected token '|'";
        return false;
    }

    size_t end = tokens.size();
    if (tokens[end - 1] == "&") {
        out.background = true;
        end--;

        if (end == 0) {
            err_msg = "syntax error near unexpected token '&'";
            return false;
        }
    }

    Command current;

    auto flush_command = [&]() -> bool {
        if (current.argv.empty()) {
            err_msg = "syntax error: missing command";
            return false;
        }
        out.pipeline.push_back(current);
        current = Command{};
        return true;
    };

    for (size_t i = 0; i < end; i++) {
        const std::string& t = tokens[i];

        if (t == "|") {
            if (!flush_command()) return false;

            if (i + 1 >= end || tokens[i + 1] == "|") {
                err_msg = "syntax error near unexpected token '|'";
                return false;
            }
            continue;
        }

        if (is_redirect_token(t)) {
            if (i + 1 >= end) {
                err_msg = "syntax error: redirection missing filename";
                return false;
            }

            const std::string& file = tokens[i + 1];

            if (is_operator_token(file)) {
                err_msg = "syntax error: redirection missing filename";
                return false;
            }

            if (t == "<") {
                current.stdin_file = file;
            } else if (t == ">") {
                current.stdout_file = file;
                current.append = false;
            } else if (t == ">>") {
                current.stdout_file = file;
                current.append = true;
            }

            i++;
            continue;
        }

        current.argv.push_back(t);
    }

    if (!flush_command()) return false;

    return true;
}


bool is_builtin(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return false;
    return (tokens[0] == "cd" || tokens[0] == "exit" || tokens[0] == "jobs");
}

static int builtin_cd(const std::vector<std::string>& tokens) {
    const char* target = nullptr;

    if (tokens.size() >= 2) {
        target = tokens[1].c_str();
    } else {
        target = std::getenv("HOME");
        if (!target) {
            std::cerr << "cd: HOME not set\n";
            return 1;
        }
    }

    if (chdir(target) != 0) {
        std::cerr << "cd: " << std::strerror(errno) << "\n";
        return 1;
    }

    return 0;
}

int run_builtin(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return 0;

    if (tokens[0] == "exit") {
        return -1;
    }

    if (tokens[0] == "cd") {
        return builtin_cd(tokens);
    }

    if (tokens[0] == "jobs") {
        print_jobs();
        return 0;
    }

    std::cerr << "Unknown builtin\n";
    return 1;
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

// reap finished/stopped/continued children
void reap_children() {
    // only do real work if a SIGCHLD happened (fast path)
    if (!g_sigchld_flag) {
        return;
    }

    // clear flag and reap everything available
    g_sigchld_flag = 0;

    int status = 0;

    while (true) {
        // reap any child that changed state
        pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);

        if (pid == 0) {
            // no more state changes available right now
            break;
        }

        if (pid < 0) {
            if (errno == ECHILD) {
                break;
            }
            std::cerr << "waitpid: " << std::strerror(errno) << "\n";
            break;
        }

        // update job table based on this pid
        for (Job& j : g_jobs) {
            if (!j.contains_pid((int)pid)) continue;

            // process exited normally or via signal -> remove from job pids
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                j.remove_pid((int)pid);

                // if job is done, print done message
                if (j.state == JobState::DONE) {
                    std::cout << "[" << j.job_id << "] Done     " << j.cmd << "\n";
                }
            }

            // process stopped -> mark job stopped
            if (WIFSTOPPED(status)) {
                j.state = JobState::STOPPED;
                std::cout << "[" << j.job_id << "] Stopped  " << j.cmd << "\n";
            }

            // process continued -> mark job running
            if (WIFCONTINUED(status)) {
                j.state = JobState::RUNNING;
            }

            break;
        }
    }
}

// wait for a foreground process group until it either finishes or stops
// returns:
// - 0 if job finished normally (at least one child exited)
// - 2 if job stopped (Ctrl-Z)
// - 1 on error
static int wait_for_foreground_job(pid_t pgid, std::vector<int>& pids, bool& stopped_out) {
    stopped_out = false;

    // wait until all pids are gone, or we detect a stop
    while (!pids.empty()) {
        int status = 0;

        // wait for any child in this process group
        pid_t w = waitpid(-pgid, &status, WUNTRACED);

        if (w < 0) {
            if (errno == EINTR) {
                // interrupted by signal, retry
                continue;
            }
            std::cerr << "waitpid: " << std::strerror(errno) << "\n";
            return 1;
        }

        // child stopped
        if (WIFSTOPPED(status)) {
            stopped_out = true;
            return 2;
        }

        // child exited: remove it from pid list
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            for (size_t i = 0; i < pids.size(); i++) {
                if (pids[i] == (int)w) {
                    pids.erase(pids.begin() + i);
                    break;
                }
            }
        }
    }

    return 0;
}

// run a single command
int run_command(const Command& cmd, bool background) {
    if (cmd.argv.empty()) {
        std::cerr << "myshell: empty command\n";
        return 1;
    }

    // build a display string for job table
    std::string cmd_display;
    for (size_t i = 0; i < cmd.argv.size(); i++) {
        if (i) cmd_display += " ";
        cmd_display += cmd.argv[i];
    }

    pid_t pid = fork();

    if (pid < 0) {
        std::cerr << "fork: " << std::strerror(errno) << "\n";
        return 1;
    }

    // child
    if (pid == 0) {
        // create process group (pgid = pid)
        if (setpgid(0, 0) < 0) {
            std::cerr << "setpgid: " << std::strerror(errno) << "\n";
        }

        // restore default signal behavior for the child
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTSTP, SIG_DFL);
        std::signal(SIGCHLD, SIG_DFL);

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
            if (cmd.append) flags |= O_APPEND;
            else flags |= O_TRUNC;

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

        std::vector<char*> argv = build_exec_argv(cmd);
        execvp(argv[0], argv.data());

        std::cerr << cmd.argv[0] << ": " << std::strerror(errno) << "\n";
        _exit(127);
    }

    // parent: set process group defensively
    setpgid(pid, pid);
    pid_t pgid = pid;

    // background: add to jobs and return immediately
    if (background) {
        std::vector<int> pids;
        pids.push_back((int)pid);

        int job_id = add_job(pgid, pids, cmd_display, JobState::RUNNING);
        print_job_started(job_id, pgid);
        return 0;
    }

    // foreground: give terminal, set fg pgid for signal forwarding, wait, reclaim
    give_terminal_to(pgid);
    g_fg_pgid = (sig_atomic_t)pgid;

    std::vector<int> pids;
    pids.push_back((int)pid);

    bool stopped = false;
    int rc = wait_for_foreground_job(pgid, pids, stopped);

    // clear foreground pgid and reclaim terminal
    g_fg_pgid = 0;
    reclaim_terminal();

    // if stopped, add to job table as STOPPED
    if (stopped) {
        std::vector<int> still_alive;
        still_alive.push_back((int)pid);

        int job_id = add_job(pgid, still_alive, cmd_display, JobState::STOPPED);
        std::cout << "[" << job_id << "] Stopped  " << cmd_display << "\n";
        return 0;
    }

    return rc;
}

// run a pipeline
int run_pipeline(const CommandLine& cmdline, bool background) {
    if (cmdline.pipeline.empty()) return 0;

    size_t n = cmdline.pipeline.size();

    // build display string for job table
    std::string cmd_display;
    for (size_t i = 0; i < cmdline.pipeline.size(); i++) {
        if (i) cmd_display += " | ";
        for (size_t k = 0; k < cmdline.pipeline[i].argv.size(); k++) {
            if (k) cmd_display += " ";
            cmd_display += cmdline.pipeline[i].argv[k];
        }
    }

    // validate redirection rules
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

    std::vector<int> pids;
    pid_t pgid = -1;

    int prev_read = -1;

    for (size_t i = 0; i < n; i++) {
        int pipefd[2] = {-1, -1};

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

        // child
        if (pid == 0) {
            // first stage becomes process group leader, later stages join pgid
            if (i == 0) {
                if (setpgid(0, 0) < 0) {
                    std::cerr << "setpgid: " << std::strerror(errno) << "\n";
                }
            } else {
                if (setpgid(0, pgid) < 0) {
                    std::cerr << "setpgid: " << std::strerror(errno) << "\n";
                }
            }

            // restore default signal behavior for the child
            std::signal(SIGINT, SIG_DFL);
            std::signal(SIGTSTP, SIG_DFL);
            std::signal(SIGCHLD, SIG_DFL);

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
                if (cmdline.pipeline[i].append) flags |= O_APPEND;
                else flags |= O_TRUNC;

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

            // exec stage command
            const Command& c = cmdline.pipeline[i];
            if (c.argv.empty()) {
                std::cerr << "myshell: empty command\n";
                _exit(1);
            }

            std::vector<char*> argv = build_exec_argv(c);
            execvp(argv[0], argv.data());

            std::cerr << c.argv[0] << ": " << std::strerror(errno) << "\n";
            _exit(127);
        }

        // parent
        if (i == 0) {
            pgid = pid;
        }

        // set process group defensively
        setpgid(pid, pgid);

        pids.push_back((int)pid);

        if (prev_read != -1) {
            close(prev_read);
            prev_read = -1;
        }

        if (i < n - 1) {
            close(pipefd[1]);
            prev_read = pipefd[0];
        }
    }

    if (prev_read != -1) {
        close(prev_read);
        prev_read = -1;
    }

    // background pipeline: add to job table and return immediately
    if (background) {
        int job_id = add_job(pgid, pids, cmd_display, JobState::RUNNING);
        print_job_started(job_id, pgid);
        return 0;
    }

    // foreground pipeline: give terminal, set fg pgid, wait, reclaim
    give_terminal_to(pgid);
    g_fg_pgid = (sig_atomic_t)pgid;

    bool stopped = false;
    int rc = wait_for_foreground_job(pgid, pids, stopped);

    g_fg_pgid = 0;
    reclaim_terminal();

    // if stopped, add remaining pids as STOPPED job
    if (stopped) {
        int job_id = add_job(pgid, pids, cmd_display, JobState::STOPPED);
        std::cout << "[" << job_id << "] Stopped  " << cmd_display << "\n";
        return 0;
    }

    return rc;
}



// none of these are used anymore
std::vector<char*> build_argv(const std::vector<std::string>& tokens) {
    std::vector<char*> argv;
    for (const std::string& s : tokens) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

int run_external(const std::vector<std::string>& tokens) {
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

    int status = 0;
    while (true) {
        pid_t w = waitpid(pid, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            std::cerr << "waitpid: " << std::strerror(errno) << "\n";
            return 1;
        }
        break;
    }
    return 0;
}
