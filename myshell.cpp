// myshell.cpp

#include "myshell.hpp"

// for error codes
#include <cerrno>
// for character classification
#include <cctype>
// to print human readable syscall failures
#include <cstring>
// input/output
#include <iostream>
// for tokenizing strings by whitespace
#include <sstream>

// needed for open() and its flags
#include <fcntl.h>

// posix signals
#include <csignal>
#include <signal.h>

// posix types
#include <sys/types.h>
// for waiting for processes
#include <sys/wait.h>
// posix operating system api
#include <unistd.h>

// shell process group id, -1 means not initialized yet -- is set in init_shell_job_control
static pid_t g_shell_pgid = -1;

// shell terminal fd (stdin is terminal)
static int g_shell_terminal_fd = STDIN_FILENO;

// set by SIGCHLD handler to tell main loop there is work to reap
static volatile sig_atomic_t g_sigchld_flag = 0;

// current foreground process group id (0 or -1 means no foreground job)
static volatile sig_atomic_t g_fg_pgid = 0;

// current and previous job ids for "+"/"-" formatting in jobs output
static int g_current_job_id = 0;
static int g_previous_job_id = 0;

/* make the process group the foreground group of terminal
 determines who receives keyboard input and terminal generated signals*/
static void give_terminal_to(pid_t pgid)
{
    // check that this is interactive, for example tcsetpgrp will fail if ran like ./myshell < script.sh
    if (!isatty(g_shell_terminal_fd))
    {
        return;
    }

    /* tcsetpgrp makes the given process group the foreground group of the terminal
     this causes terminal control to be per process group, not by pid*/
    if (tcsetpgrp(g_shell_terminal_fd, pgid) < 0)
    {
        std::cerr << "tcsetpgrp: " << std::strerror(errno) << "\n";
    }
}

/* restore the terminal's foreground process back to shell itself after foreground job stops or exits
essential so shell does not remain in background and so reading input works*/
static void reclaim_terminal()
{
    // check that this is interactive
    if (!isatty(g_shell_terminal_fd))
    {
        return;
    }
    // check that shell pgid is valid
    if (g_shell_pgid < 0)
    {
        return;
    }
    // set shell to foreground group of process
    if (tcsetpgrp(g_shell_terminal_fd, g_shell_pgid) < 0)
    {
        std::cerr << "tcsetpgrp: " << std::strerror(errno) << "\n";
    }
}

/* initialize shell process group and attach to terminal
 essential so ctrl-c and ctrl-z is handled correctly, along with fg and bg*/
void init_shell_job_control()
{
    // check that this is interactive
    if (!isatty(g_shell_terminal_fd))
    {
        return;
    }

    // shell process group id is the shell pid
    g_shell_pgid = getpid();

    /* setpgid(0,0) means this process uses its own pid as the pgid
    essential so foreground/background control only works on process groups
     shell cannot share a process group with its parent*/
    if (setpgid(0, 0) < 0)
    {
        std::cerr << "setpgid: " << std::strerror(errno) << "\n";
    }

    // makes shell the process group owner the foreground owner of terminal
    if (tcsetpgrp(g_shell_terminal_fd, g_shell_pgid) < 0)
    {
        std::cerr << "tcsetpgrp: " << std::strerror(errno) << "\n";
    }

    /* ignore SIGTTOU so tcsetpgrp does not stop the shell
     SIGTTOU is sent when background process tries to write to terminal or call tcsetpgrp
     but shell must call tcsetpgrp when in background to reclaim the terminal
    these ignores prevent the shell from stopping itself*/
    std::signal(SIGTTOU, SIG_IGN);
    /*SIGTTIN is sent when background process tries to read from terminal, ingore it so
    the shell does not stop itself*/
    std::signal(SIGTTIN, SIG_IGN);
}

// well defined set of 3 states of jobs
enum class JobState
{
    RUNNING,
    STOPPED,
    DONE
};

// represents one shell job, could be one or more processes (a job is a process group)
class Job
{
public:
    // numeric job id: [1], [2],...,[n]
    int job_id = 0;

    // process group id for the job
    pid_t pgid = -1;

    // list of pids belonging to this job (pipelines have multiple)
    std::vector<pid_t> pids;

    // the command line text (for display like [1] Done [command])
    std::string cmd;

    // job state based on the enum set
    JobState state = JobState::RUNNING;

    // check if pid is part of this job
    bool contains_pid(int pid) const
    {
        for (int p : pids)
        {
            if (p == pid)
            {
                return true;
            }
        }
        return false;
    }
    // remove a pid when it has terminated
    void remove_pid(int pid)
    {
        for (size_t i = 0; i < pids.size(); i++)
        {
            if (pids[i] == pid)
            {
                pids.erase(pids.begin() + i);
                break;
            }
        }
        // if no pids left, job is done
        if (pids.empty())
        {
            state = JobState::DONE;
        }
    }
};

// global job table
static std::vector<Job> g_jobs;

// next job id counter, this shell does not reuse ids for simplicity
static int g_next_job_id = 1;

/* computes which job is current vs previous based on most recent active jobs
called when job is added, removed, or state changes*/
static void recompute_job_marks()
{
    // clears old state before recomputing
    g_current_job_id = 0;
    g_previous_job_id = 0;

    // the current job is the last one in the vector
    if (!g_jobs.empty())
    {
        g_current_job_id = g_jobs.back().job_id;
    }
    // the previous one is the second to last job in the vector
    if (g_jobs.size() >= 2)
    {
        g_previous_job_id = g_jobs[g_jobs.size() - 2].job_id;
    }
}

// add job to the job table
static int add_job(pid_t pgid, const std::vector<pid_t> &pids, const std::string &cmd, JobState st)
{
    Job j;
    // set the fields of the job
    j.job_id = g_next_job_id++;
    j.pgid = pgid;
    j.pids = pids;
    j.cmd = cmd;
    j.state = st;

    // add job to vector
    g_jobs.push_back(j);

    // update current and previous
    g_previous_job_id = g_current_job_id;
    g_current_job_id = j.job_id;
    // return the job id
    return j.job_id;
}

// print a job launch line
static void print_job_started(int job_id, pid_t pgid)
{
    std::cout << "[" << job_id << "] " << pgid << "\n";
}

// return marker for jobs output
// returns a +/- or space depending on current/previous jobs
static char job_marker(int job_id)
{
    if (job_id == g_current_job_id)
    {
        return '+';
    }
    if (job_id == g_previous_job_id)
    {
        return '-';
    }
    return ' ';
}

// print jobs builtin output
static void print_jobs()
{
    // go through the jobs and add marks if needed
    for (const Job &j : g_jobs)
    {

        char mark = job_marker(j.job_id);
        // print job with correct state
        if (j.state == JobState::RUNNING)
        {
            std::cout << "[" << j.job_id << "]" << mark << " Running  " << j.cmd << "\n";
        }
        else if (j.state == JobState::STOPPED)
        {
            std::cout << "[" << j.job_id << "]" << mark << " Stopped  " << j.cmd << "\n";
        }
    }
}

// find a job index by its id
static int find_job_index_by_id(int job_id)
{
    // cast to int because size is a size_t which is unsigned integer
    for (int i = 0; i < (int)g_jobs.size(); i++)
    {
        if (g_jobs[i].job_id == job_id)
        {
            return i;
        }
    }
    return -1;
}

/* parse a job id token for things like fg [job id] or bg %[job id]
return true if valid, false if not parsed*/
static bool parse_job_id_token(const std::string &tok, int &out_id)
{
    std::string s = tok;

    // allow leading %, just erase it if there
    if (!s.empty() && s[0] == '%')
    {
        s.erase(s.begin());
    }
    // if empty just return because there is nothing to parse
    if (s.empty())
    {
        return false;
    }

    // make sure it is digits, if not return false
    for (char c : s)
    {
        if (!std::isdigit((unsigned char)c))
        {
            return false;
        }
    }

    // the id is the integer value of the token string
    out_id = std::stoi(s);
    return true;
}

// choose default job for fg
static int pick_default_job_for_fg()
{
    // most recent stopped job, if none then most recent running job
    for (int i = (int)g_jobs.size() - 1; i >= 0; i--)
    {
        if (g_jobs[i].state == JobState::STOPPED)
        {
            return i;
        }
    }
    for (int i = (int)g_jobs.size() - 1; i >= 0; i--)
    {
        if (g_jobs[i].state == JobState::RUNNING)
        {
            return i;
        }
    }
    return -1;
}

// choose default job for bg
static int pick_default_job_for_bg()
{
    // prefer most recent stopped job
    for (int i = (int)g_jobs.size() - 1; i >= 0; i--)
    {
        if (g_jobs[i].state == JobState::STOPPED)
        {
            return i;
        }
    }
    return -1;
}

/* SIGCHILD handler, set a flag, SIGCHILD is when any child changes state
empty param because handler must accept the signal number but we dont need it*/
static void sigchld_handler(int)
{
    g_sigchld_flag = 1;
}

// SIGINT/SIGTSTP handler: forward the signal foreground process group if one exists
static void forward_to_foreground(int sig)
{
    // cast from sigatomic to pid_t
    pid_t pgid = (pid_t)g_fg_pgid;

    // if no foreground job, do nothing
    if (pgid <= 0)
    {
        return;
    }

    /* send signal to the entire foreground process group with kill
     negative pid means process group */
    kill(-pgid, sig);
}
// SIGINT is caused by ctrl-c, call forward to foreground
static void sigint_handler(int sig)
{
    forward_to_foreground(sig);
}
// SIGTSP is caused by ctrl-z, call forward to foreground
static void sigtstp_handler(int sig)
{
    forward_to_foreground(sig);
}

// set signal handlers using sigaction so we can control SA_RESTART
void init_shell_signals()
{
    // use blocks to limit scope to reuse names like sa
    // SIGCHLD: notify main loop to reap children
    {
        struct sigaction sa;
        // clears all fields so there are no garbage values
        std::memset(&sa, 0, sizeof(sa));

        // tells the kernel to call the sigchld_handler function when SIGCHLD
        sa.sa_handler = sigchld_handler;

        /* do not set SA_RESTART: we want blocking input to return with EINTR so we can reap and print Done
        solves the issue of Done only printing after another command is entered
        more specifically, this line tells kernel to not automatically restart read if SIGCHLD, make it fail with EINTR
        this causes the main loop to wake up and call reap_children. this line isnt needed after the memset
        but it is an important thing to note*/
        sa.sa_flags = 0;

        /*mask is set of signals blocked while handler runs, empty it so extra signals dont get blocked,
        also not needed but important thing to note*/
        sigemptyset(&sa.sa_mask);

        // error check if installing the signal handler failed
        if (sigaction(SIGCHLD, &sa, nullptr) < 0)
        {
            std::cerr << "sigaction(SIGCHLD): " << std::strerror(errno) << "\n";
        }
    }

    // SIGINT: forward Ctrl-C to foreground job (shell stays alive)
    {
        struct sigaction sa;
        // clears all fields so there are no garbage values
        std::memset(&sa, 0, sizeof(sa));

        // tells the kernel to call the sigint_handler function when SIGINT
        sa.sa_handler = sigint_handler;

        /*mask is set of signals blocked while handler runs, empty it so extra signals dont get blocked,
        also not needed but important thing to note*/
        sigemptyset(&sa.sa_mask);

        // error check if installing the signal handler failed
        if (sigaction(SIGINT, &sa, nullptr) < 0)
        {
            std::cerr << "sigaction(SIGINT): " << std::strerror(errno) << "\n";
        }
    }

    // SIGTSTP: forward Ctrl-Z to foreground job
    {
        struct sigaction sa;
        // clears all fields so there are no garbage values
        std::memset(&sa, 0, sizeof(sa));

        // tells the kernel to call the sigtstp_handler function when SIGTSTP
        sa.sa_handler = sigtstp_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);

        if (sigaction(SIGTSTP, &sa, nullptr) < 0)
        {
            std::cerr << "sigaction(SIGTSTP): " << std::strerror(errno) << "\n";
        }
    }

    /* ignore SIGQUIT, we can use signal for simplicity instead of sigaction,
    we ignore this because we do not want the user to lose the shell session*/
    std::signal(SIGQUIT, SIG_IGN);
}

void print_prompt()
{
    // print prompt and flush output so it appears immediately
    std::cout << "myshell> " << std::flush;
}

// this is a signal aware replacement for std::getline, true means keep shell running, false means exit shell
bool read_line(std::string &line)
{
    // clear output line so no leftovers from previous reads
    line.clear();

    // read input one byte at a time until newline or EOF
    while (true)
    {
        char c = 0;

        // read 1 byte from stdin, use ssize because it can be negative if error
        ssize_t n = read(STDIN_FILENO, &c, 1);

        // EOF: user pressed Ctrl-D on an empty line
        if (n == 0)
        {
            // if we already collected characters, treat it as a final line, else exit shell
            if (!line.empty())
            {
                return true;
            }
            return false;
        }

        // read error
        if (n < 0)
        {
            // interrupted by signal: return to main loop so it can reap children
            if (errno == EINTR)
            {
                return true;
            }

            // other errors
            std::cerr << "read: " << std::strerror(errno) << "\n";
            return true;
        }

        // newline ends the line
        if (c == '\n')
        {
            return true;
        }

        // normal character: append to string
        line.push_back(c);
    }
}

// check for operator
static bool is_operator_char(char c)
{
    // operators are single characters except for >>
    return (c == '|' || c == '<' || c == '>' || c == '&');
}

// tokenize the operators, takes in a full command line and returns vector of tokens
std::vector<std::string> tokenize_operators(const std::string &line)
{
    std::vector<std::string> tokens;

    // current token being built
    std::string cur;

    // helper lambda to push the current token to vector if non-empty
    auto flush_cur = [&]()
    {
        if (!cur.empty())
        {
            tokens.push_back(cur);
            cur.clear();
        }
    };

    // scan line character by character
    for (size_t i = 0; i < line.size(); i++)
    {
        char c = line[i];

        // treat whitespace, tabs, and new lines as delimiters
        if (c == ' ' || c == '\t' || c == '\n')
        {
            flush_cur();
            continue;
        }

        // handle operators
        if (is_operator_char(c))
        {
            flush_cur();

            // special case: >> is one token
            if (c == '>' && (i + 1) < line.size() && line[i + 1] == '>')
            {
                tokens.push_back(">>");
                i++;
                // this handles | < > and &
            }
            else
            {
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

// helper to see if a token list contains any operators
bool contains_operators(const std::vector<std::string> &tokens)
{
    for (const std::string &t : tokens)
    {
        if (t == "|" || t == "<" || t == ">" || t == ">>" || t == "&")
        {
            return true;
        }
    }
    return false;
}

// return true if input redirection and stdin_file
bool Command::has_stdin() const
{
    return !stdin_file.empty();
}

// retirn true if output redirection and stdout_file
bool Command::has_stdout() const
{
    return !stdout_file.empty();
}

// returns true if more than one command stage
bool CommandLine::is_pipeline() const
{
    return pipeline.size() > 1;
}

// return true if it is a redirect token
static bool is_redirect_token(const std::string &t)
{
    return (t == "<" || t == ">" || t == ">>");
}

// return true if it is an operator token
static bool is_operator_token(const std::string &t)
{
    return (t == "|" || t == "<" || t == ">" || t == ">>" || t == "&");
}

// parse the command line, takes in tokens, parsed CommandLine and err msg reference, returns false if error
bool parse_command_line(const std::vector<std::string> &tokens, CommandLine &out, std::string &err_msg)
{
    // clears previous results
    out = CommandLine{};
    err_msg.clear();

    // if empty return false
    if (tokens.empty())
    {
        err_msg = "empty command";
        return false;
    }

    // if leading pipe return false
    if (tokens[0] == "|")
    {
        err_msg = "syntax error near unexpected token '|'";
        return false;
    }

    // handle background operator
    size_t end = tokens.size();
    if (tokens[end - 1] == "&")
    {
        out.background = true;
        // shrinks range so & is not treated as a token
        end--;
        // handles bare &
        if (end == 0)
        {
            err_msg = "syntax error near unexpected token '&'";
            return false;
        }
    }

    // current command being built, holds argv, stdin/out files and if append
    Command current;

    // lambda to finalize current command and add to command line
    auto flush_command = [&]() -> bool
    {
        if (current.argv.empty())
        {
            err_msg = "syntax error: missing command";
            return false;
        }
        out.pipeline.push_back(current);
        current = Command{};
        return true;
    };

    // go through each token
    for (size_t i = 0; i < end; i++)
    {
        const std::string &t = tokens[i];

        // for pipe try to flush command
        if (t == "|")
        {
            if (!flush_command())
            {
                return false;
            }
            //  rejects invalid pipe placement
            if (i + 1 >= end || tokens[i + 1] == "|")
            {
                err_msg = "syntax error near unexpected token '|'";
                return false;
            }
            continue;
        }

        // redirection handling
        if (is_redirect_token(t))
        {
            // rejects invalid redirection placement
            if (i + 1 >= end)
            {
                err_msg = "syntax error: redirection missing filename";
                return false;
            }

            // set file
            const std::string &file = tokens[i + 1];

            // filename cannot be another operator
            if (is_operator_token(file))
            {
                err_msg = "syntax error: redirection missing filename";
                return false;
            }

            // set the appropriate fields based on type of redirection
            if (t == "<")
            {
                current.stdin_file = file;
            }
            else if (t == ">")
            {
                current.stdout_file = file;
                current.append = false;
            }
            else if (t == ">>")
            {
                current.stdout_file = file;
                current.append = true;
            }
            // move to next token -- skips filename
            i++;
            continue;
        }
        // add normal arguments to the line of commands
        current.argv.push_back(t);
    }
    // push last command before loop ends
    if (!flush_command())
    {
        return false;
    }

    return true;
}

// build argv for execvp from Command.argv, must be C-style char* argv[] arrays
static std::vector<char *> build_exec_argv(const Command &cmd)
{
    std::vector<char *> argv;
    // loop over strings in cmd.argv
    for (const std::string &s : cmd.argv)
    {
        // convert string to char* and casts to char* (not const)
        argv.push_back(const_cast<char *>(s.c_str()));
    }
    // argv must be null terminated
    argv.push_back(nullptr);
    return argv;
}

// helper: remove a job by index and fix +/-
static void erase_job_by_index(int idx)
{
    // bounds check
    if (idx < 0 || idx >= (int)g_jobs.size())
    {
        return;
    }
    // save job id before deletion
    int removed_id = g_jobs[idx].job_id;
    // remove the job
    g_jobs.erase(g_jobs.begin() + idx);

    // if removed job affected markers, recompute
    if (removed_id == g_current_job_id || removed_id == g_previous_job_id)
    {
        recompute_job_marks();
    }
}

// remove all DONE jobs
static void cleanup_done_jobs()
{
    // iterate throught and if the job is DONE then erase it by index
    for (int i = (int)g_jobs.size() - 1; i >= 0; i--)
    {
        if (g_jobs[i].state == JobState::DONE)
        {
            erase_job_by_index(i);
        }
    }
}

// reap finished/stopped/continued children
void reap_children()
{
    // only do real work if a SIGCHLD happened
    if (!g_sigchld_flag)
    {
        return;
    }

    // reset flag and reap everything available
    g_sigchld_flag = 0;

    // status for waitpid
    int status = 0;

    // reap everything that changed
    while (true)
    {
        // reap any child that changed state, -1 means any child process
        pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);

        // no child has a pending state change right now so break
        if (pid == 0)
        {
            break;
        }

        // handle errors
        if (pid < 0)
        {
            // ECHILD means no children
            if (errno == ECHILD)
            {
                break;
            }
            std::cerr << "waitpid: " << std::strerror(errno) << "\n";
            break;
        }

        // update job table based on this pid
        for (int ji = 0; ji < (int)g_jobs.size(); ji++)
        {
            Job &j = g_jobs[ji];
            if (!j.contains_pid(pid))
            {
                continue;
            }

            // process exited normally or via signal then remove from job pids
            if (WIFEXITED(status) || WIFSIGNALED(status))
            {
                j.remove_pid(pid);

                // if job is done, print done message once and remove job immediately
                if (j.state == JobState::DONE)
                {
                    char mark = job_marker(j.job_id);
                    std::cout << "[" << j.job_id << "]" << mark << " Done     " << j.cmd << "\n";
                    erase_job_by_index(ji);
                }
            }

            // process stopped then mark job stopped and print
            if (WIFSTOPPED(status))
            {
                j.state = JobState::STOPPED;
                char mark = job_marker(j.job_id);
                std::cout << "[" << j.job_id << "]" << mark << " Stopped  " << j.cmd << "\n";
            }

            // process continued then mark job running
            if (WIFCONTINUED(status))
            {
                j.state = JobState::RUNNING;
            }
            // break out once the pid job was found
            break;
        }
    }

    // ensure no DONE jobs remain
    cleanup_done_jobs();
}

/*wait for a foreground process group until it either finished or stops,
returns 0 if finished, 2 if stopped, 1 on error*/
static int wait_for_foreground_job(pid_t pgid, std::vector<pid_t> &pids, bool &stopped_out)
{
    stopped_out = false;

    // wait until all pids in job are gone, or we detect a stop
    while (!pids.empty())
    {
        int status = 0;

        // wait for any child in this process group, -pgid means process group, WUNTRACED reports stops too
        pid_t w = waitpid(-pgid, &status, WUNTRACED);

        // if error
        if (w < 0)
        {
            if (errno == EINTR)
            {
                // interrupted by signal, retry
                continue;
            }
            std::cerr << "waitpid: " << std::strerror(errno) << "\n";
            return 1;
        }

        // child stopped so set flag and return
        if (WIFSTOPPED(status))
        {
            stopped_out = true;
            return 2;
        }

        // child exited: remove it from pid list
        if (WIFEXITED(status) || WIFSIGNALED(status))
        {
            for (size_t i = 0; i < pids.size(); i++)
            {
                if (pids[i] == w)
                {
                    pids.erase(pids.begin() + i);
                    break;
                }
            }
        }
    }

    return 0;
}

// check if the tokens are built in, this is so these commands have effect on shell, not just child
bool is_builtin(const std::vector<std::string> &tokens)
{
    // if no commands return false
    if (tokens.empty())
    {
        return false;
    }
    // small set of commands built into the shell
    return (tokens[0] == "cd" || tokens[0] == "exit" || tokens[0] == "jobs" ||
            tokens[0] == "fg" || tokens[0] == "bg");
}

// handles changing directory
static int builtin_cd(const std::vector<std::string> &tokens)
{
    // hold directory to switch to, const char* because chdir expects C-string
    const char *target = nullptr;

    // if directory is given then tokens[1] is directory
    if (tokens.size() >= 2)
    {
        target = tokens[1].c_str();
        // if directory is not given then default to $HOME
    }
    else
    {
        target = std::getenv("HOME");
        // if no target then no home
        if (!target)
        {
            std::cerr << "cd: HOME not set\n";
            return 1;
        }
    }

    // change directory with syscall
    if (chdir(target) != 0)
    {
        std::cerr << "cd: " << std::strerror(errno) << "\n";
        return 1;
    }

    return 0;
}

// bring a job into the foreground
static int builtin_fg(const std::vector<std::string> &tokens)
{
    // reap children and cleanup done jobs before making changes
    reap_children();
    cleanup_done_jobs();
    // choose target job
    int job_index = -1;

    // case where argument is given, e.g. fg %1 or fg 1
    if (tokens.size() >= 2)
    {
        int job_id = 0;
        // if job id does not exist then return
        if (!parse_job_id_token(tokens[1], job_id))
        {
            std::cerr << "fg: invalid job id\n";
            return 1;
        }

        // find the job, if cant be found then return
        job_index = find_job_index_by_id(job_id);
        if (job_index < 0)
        {
            std::cerr << "fg: no such job\n";
            return 1;
        }
        // case with no argument
    }
    else
    {
        // just pick default job for fg
        job_index = pick_default_job_for_fg();
        if (job_index < 0)
        {
            std::cerr << "fg: no current job\n";
            return 1;
        }
    }
    // grab the job object
    Job &j = g_jobs[job_index];

    // print the command like bash does when you fg something
    std::cout << j.cmd << "\n";

    // give terminal to the job's process group
    give_terminal_to(j.pgid);

    // set foreground pgid for signal forwarding
    g_fg_pgid = (sig_atomic_t)j.pgid;

    // continue the job, -pgid means process group
    if (kill(-j.pgid, SIGCONT) < 0)
    {
        std::cerr << "kill(SIGCONT): " << std::strerror(errno) << "\n";
    }

    // mark running
    j.state = JobState::RUNNING;

    // wait for the job in the foreground
    bool stopped = false;
    // waits for whole group and removes pids as they exit, sets stopped to true if stop is detected
    int rc = wait_for_foreground_job(j.pgid, j.pids, stopped);

    // clear foreground pgid and reclaim terminal
    g_fg_pgid = 0;
    reclaim_terminal();

    // if it stopped, keep it in job table as STOPPED
    if (stopped)
    {
        j.state = JobState::STOPPED;
        char mark = job_marker(j.job_id);
        std::cout << "[" << j.job_id << "]" << mark << " Stopped  " << j.cmd << "\n";
        return 0;
    }

    // finished in foreground: remove it from job table
    if (j.pids.empty())
    {
        erase_job_by_index(job_index);
    }

    return rc;
}

// resume a stopped job in the background
static int builtin_bg(const std::vector<std::string> &tokens)
{
    // reap children and cleanup done jobs before making changes
    reap_children();
    cleanup_done_jobs();
    // choose target job
    int job_index = -1;

    // case where argument is given, e.g. bg %1 or bg 1
    if (tokens.size() >= 2)
    {
        int job_id = 0;
        // if job id does not exist then return
        if (!parse_job_id_token(tokens[1], job_id))
        {
            std::cerr << "bg: invalid job id\n";
            return 1;
        }

        // find the job, if cant be found then return
        job_index = find_job_index_by_id(job_id);
        if (job_index < 0)
        {
            std::cerr << "bg: no such job\n";
            return 1;
        }
        // case with no argument
    }
    else
    {
        // just pick default job for bg
        job_index = pick_default_job_for_bg();
        if (job_index < 0)
        {
            std::cerr << "bg: no current job\n";
            return 1;
        }
    }

    // grab the job object
    Job &j = g_jobs[job_index];

    // bg only makes sense for stopped jobs
    if (j.state != JobState::STOPPED)
    {
        std::cerr << "bg: job is not stopped\n";
        return 1;
    }

    // continue the job in background, -pgid means process group
    if (kill(-j.pgid, SIGCONT) < 0)
    {
        std::cerr << "kill(SIGCONT): " << std::strerror(errno) << "\n";
        return 1;
    }

    // set state to running
    j.state = JobState::RUNNING;

    // print running status
    char mark = job_marker(j.job_id);
    std::cout << "[" << j.job_id << "]" << mark << " Running  " << j.cmd << "\n";

    return 0;
}

// run built in shell commands, returns 0 for success, -1 to terminate shell and >0 for error
int run_builtin(const std::vector<std::string> &tokens)
{
    // check that there are tokens
    if (tokens.empty())
    {
        return 0;
    }

    // returning -1 tells shell to exit
    if (tokens[0] == "exit")
    {
        return -1;
    }

    // runs the builtin_cd function
    if (tokens[0] == "cd")
    {
        return builtin_cd(tokens);
    }

    // handles job listing
    if (tokens[0] == "jobs")
    {
        // before showing jobs, ensure no DONE jobs remain
        cleanup_done_jobs();
        print_jobs();
        return 0;
    }

    // brings process group to fg
    if (tokens[0] == "fg")
    {
        return builtin_fg(tokens);
    }

    // sends process group to bg
    if (tokens[0] == "bg")
    {
        return builtin_bg(tokens);
    }

    // failsafe for errors
    std::cerr << "Unknown builtin\n";
    return 1;
}

// run a single command, either in foreground or background
int run_command(const Command &cmd, bool background)
{
    // check that the command contains argv
    if (cmd.argv.empty())
    {
        std::cerr << "myshell: empty command\n";
        return 1;
    }

    // build a display string for job table
    std::string cmd_display;
    for (int i = 0; i < (int)cmd.argv.size(); i++)
    {
        if (i != 0)
        {
            cmd_display += " ";
        }
        cmd_display += cmd.argv[i];
    }

    // fork the process
    pid_t pid = fork();

    // if fork fails
    if (pid < 0)
    {
        std::cerr << "fork: " << std::strerror(errno) << "\n";
        return 1;
    }

    // if pid is 0 then we are in the child process
    if (pid == 0)
    {
        // put child it its own process group, setpgid(0,0) means set my pgid to my pid
        if (setpgid(0, 0) < 0)
        {
            std::cerr << "setpgid: " << std::strerror(errno) << "\n";
        }

        // restore default signal behavior for the child because child processes should behave like normal programs
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTSTP, SIG_DFL);
        std::signal(SIGCHLD, SIG_DFL);

        // apply input redirection
        if (cmd.has_stdin())
        {
            // open the file as read only
            int fd = open(cmd.stdin_file.c_str(), O_RDONLY);
            // if opening failed
            if (fd < 0)
            {
                std::cerr << cmd.stdin_file << ": " << std::strerror(errno) << "\n";
                // _exit instead of exit for child process so it terminates immediately
                _exit(1);
            }
            // use dup2 to set the file to be the standard input for the child
            if (dup2(fd, STDIN_FILENO) < 0)
            {
                std::cerr << "dup2: " << std::strerror(errno) << "\n";
                close(fd);
                // _exit instead of exit for child process so it terminates immediately
                _exit(1);
            }
            // close the file
            close(fd);
        }

        // apply output redirection
        if (cmd.has_stdout())
        {
            // build flags, first with write only and create permissions
            int flags = O_WRONLY | O_CREAT;
            // if append flag then add append flag
            if (cmd.append)
            {
                flags |= O_APPEND;
            }
            // else add truncate flag
            else
            {
                flags |= O_TRUNC;
            }

            // open the file with the flags, 0644 is rw-r--r-- permissions, only owner can write, everyone else can read
            int fd = open(cmd.stdout_file.c_str(), flags, 0644);
            // if file fails to open
            if (fd < 0)
            {
                std::cerr << cmd.stdout_file << ": " << std::strerror(errno) << "\n";
                // _exit instead of exit for child process so it terminates immediately
                _exit(1);
            }

            // use dup2 to set the file to be standard output of the child
            if (dup2(fd, STDOUT_FILENO) < 0)
            {
                std::cerr << "dup2: " << std::strerror(errno) << "\n";
                close(fd);
                // _exit instead of exit for child process so it terminates immediately
                _exit(1);
            }
            // close the file
            close(fd);
        }

        // build the argv array, must be C-style strings for execvp
        std::vector<char *> argv = build_exec_argv(cmd);
        // run execvp to start new process
        execvp(argv[0], argv.data());

        // if this runs then there is error
        std::cerr << cmd.argv[0] << ": " << std::strerror(errno) << "\n";
        // _exit instead of exit for child process so it terminates immediately, 127 means command not found
        _exit(127);
    }

    // parent code
    /* set process group, this already happens in child, but just incase parent runs first,
    setpgid puts the child process into a process group with a pgid of pid*/
    setpgid(pid, pid);
    // record job process group id
    pid_t pgid = pid;

    // if background then add to jobs and return immediately
    if (background)
    {
        std::vector<pid_t> pids;
        // for single command there is only one pid
        pids.push_back(pid);
        // adds job to job table
        int job_id = add_job(pgid, pids, cmd_display, JobState::RUNNING);
        // prit that the job started
        print_job_started(job_id, pgid);
        return 0;
    }

    // foreground code
    // give terminal to the process group
    give_terminal_to(pgid);
    // set fg pgid for signal forwarding
    g_fg_pgid = (sig_atomic_t)pgid;

    std::vector<pid_t> pids;
    // for single command there is only one pid
    pids.push_back(pid);

    // wait for the foreground job to exit or stop
    bool stopped = false;
    int rc = wait_for_foreground_job(pgid, pids, stopped);

    // clear foreground pgid and reclaim the terminal
    g_fg_pgid = 0;
    reclaim_terminal();

    // if stopped, add to job table as STOPPED
    if (stopped)
    {
        std::vector<pid_t> still_alive;
        // for single command there is only one pid
        still_alive.push_back(pid);

        // add the job to the table with a stopped state
        int job_id = add_job(pgid, still_alive, cmd_display, JobState::STOPPED);
        // print the job as stopped
        char mark = job_marker(job_id);
        std::cout << "[" << job_id << "]" << mark << " Stopped  " << cmd_display << "\n";
        return 0;
    }

    return rc;
}

// run a pipeline, background if needed
int run_pipeline(const CommandLine &cmdline, bool background)
{
    // if no commands then do nothing
    if (cmdline.pipeline.empty())
    {
        return 0;
    }

    // find size of the command pipeline
    size_t n = cmdline.pipeline.size();

    // build display string for job table
    std::string cmd_display;
    for (size_t i = 0; i < cmdline.pipeline.size(); i++)
    {
        if (i != 0)
        {
            cmd_display += " | ";
        }
        for (size_t k = 0; k < cmdline.pipeline[i].argv.size(); k++)
        {
            if (k != 0)
            {
                cmd_display += " ";
            }
            cmd_display += cmdline.pipeline[i].argv[k];
        }
    }

    // validate redirection rules, can only be on first pipeline stage, this is specific to my shell for simplicity
    for (int i = 0; i < (int)n; i++)
    {
        if (i != 0 && cmdline.pipeline[i].has_stdin())
        {
            std::cerr << "myshell: input redirection only allowed on first pipeline stage\n";
            return 1;
        }
        if (i != (int)(n - 1) && cmdline.pipeline[i].has_stdout())
        {
            std::cerr << "myshell: output redirection only allowed on last pipeline stage\n";
            return 1;
        }
    }

    std::vector<pid_t> pids;
    // -1 is default
    pid_t pgid = -1;
    int prev_read = -1;

    // iterate through the pipeline
    for (int i = 0; i < (int)n; i++)
    {
        int pipefd[2] = {-1, -1};

        if (i < (int)(n - 1))
        {
            // use pipe to create read end and write end to form a pipeline
            if (pipe(pipefd) < 0)
            {
                std::cerr << "pipe: " << std::strerror(errno) << "\n";
                return 1;
            }
        }
        // fork the process to create a child
        pid_t pid = fork();

        // if fork failed
        if (pid < 0)
        {
            std::cerr << "fork: " << std::strerror(errno) << "\n";
            return 1;
        }

        // if pid is 0 we are in the child
        if (pid == 0)
        {
            // first stage becomes process group leader, later stages join pgid
            if (i == 0)
            {
                // setpgid(0,0) means set the pgid to the pid
                if (setpgid(0, 0) < 0)
                {
                    std::cerr << "setpgid: " << std::strerror(errno) << "\n";
                }
                // later stages
            }
            else
            {
                // setpgit(0, pgid) means set the pgid to pgid
                if (setpgid(0, pgid) < 0)
                {
                    std::cerr << "setpgid: " << std::strerror(errno) << "\n";
                }
            }

            // restore default signal behavior for the child because child processes should behave like normal programs
            std::signal(SIGINT, SIG_DFL);
            std::signal(SIGTSTP, SIG_DFL);
            std::signal(SIGCHLD, SIG_DFL);

            // use dup2 to set stdin to be prev_read, only do for stages after the first one
            if (i > 0)
            {
                if (dup2(prev_read, STDIN_FILENO) < 0)
                {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    // _exit instead of exit for child process so it terminates immediately
                    _exit(1);
                }
            }

            // use dup2 to set stdout to be the write end of pipe, do for all stages before last one
            if (i < (int)(n - 1))
            {
                if (dup2(pipefd[1], STDOUT_FILENO) < 0)
                {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    // _exit instead of exit for child process so it terminates immediately
                    _exit(1);
                }
            }

            // close unused fds
            if (prev_read != -1)
            {
                close(prev_read);
            }
            if (pipefd[0] != -1)
            {
                close(pipefd[0]);
            }
            if (pipefd[1] != -1)
            {
                close(pipefd[1]);
            }

            // apply input redirection on first stage
            if (i == 0 && cmdline.pipeline[i].has_stdin())
            {
                // open file with readonly
                int fd = open(cmdline.pipeline[i].stdin_file.c_str(), O_RDONLY);
                // if open failed
                if (fd < 0)
                {
                    std::cerr << cmdline.pipeline[i].stdin_file << ": " << std::strerror(errno) << "\n";
                    // _exit instead of exit for child process so it terminates immediately
                    _exit(1);
                }
                // set the file to be the stdin using dup2
                if (dup2(fd, STDIN_FILENO) < 0)
                {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    close(fd);
                    // _exit instead of exit for child process so it terminates immediately
                    _exit(1);
                }
                // close the file
                close(fd);
            }

            // apply output redirection on last stage
            if (i == (int)(n - 1) && cmdline.pipeline[i].has_stdout())
            {
                // build flags, start with write only and create
                int flags = O_WRONLY | O_CREAT;
                // if append then add append flag
                if (cmdline.pipeline[i].append)
                {
                    flags |= O_APPEND;
                } // otherwise add the truncate flag
                else
                {
                    flags |= O_TRUNC;
                }
                // open the file with the flags and 0644 which is rw-r--r permissions, owner can write, everyone else can read
                int fd = open(cmdline.pipeline[i].stdout_file.c_str(), flags, 0644);
                // if open failed
                if (fd < 0)
                {
                    std::cerr << cmdline.pipeline[i].stdout_file << ": " << std::strerror(errno) << "\n";
                    // _exit instead of exit for child process so it terminates immediately
                    _exit(1);
                }
                // use dup2 to set the file to be stdout
                if (dup2(fd, STDOUT_FILENO) < 0)
                {
                    std::cerr << "dup2: " << std::strerror(errno) << "\n";
                    close(fd);
                    // _exit instead of exit for child process so it terminates immediately
                    _exit(1);
                }
                // close the file
                close(fd);
            }

            // exec stage command
            const Command &c = cmdline.pipeline[i];
            // make sure argv is not empty
            if (c.argv.empty())
            {
                std::cerr << "myshell: empty command\n";
                // _exit instead of exit for child process so it terminates immediately
                _exit(1);
            }
            // build the argv array, must be C-style string
            std::vector<char *> argv = build_exec_argv(c);
            // call execvp to run the command
            execvp(argv[0], argv.data());

            // if we get here something went wrong
            std::cerr << c.argv[0] << ": " << std::strerror(errno) << "\n";
            // _exit instead of exit for child process so it terminates immediately, 127 means command not found
            _exit(127);
        }

        // parent code
        if (i == 0)
        {
            // set the pgid to be pid
            pgid = pid;
        }

        /* set process group, this already happens in child, but just incase parent runs first,
        setpgid puts the child process into a process group with a pgid of pid*/
        setpgid(pid, pgid);

        // add the pid to the vector of pids
        pids.push_back(pid);

        // if needed then close previous read and reset it to -1
        if (prev_read != -1)
        {
            close(prev_read);
            prev_read = -1;
        }

        // if we are not at the end then close the write end and make prev read the read end
        if (i < int(n - 1))
        {
            close(pipefd[1]);
            prev_read = pipefd[0];
        }
    }
    // if needed then close prev read and reset it to -1
    if (prev_read != -1)
    {
        close(prev_read);
        prev_read = -1;
    }

    // background pipeline: add to job table and return immediately
    // if this is background
    if (background)
    {
        // add the job to the table and return immediately
        int job_id = add_job(pgid, pids, cmd_display, JobState::RUNNING);
        // print that job started
        print_job_started(job_id, pgid);
        return 0;
    }

    // foreground pipeline: give terminal, set fg pgid, wait, reclaim
    // if we are in foreground
    // give the terminal to the process group
    give_terminal_to(pgid);
    // set the fg pgid for signal forwarding
    g_fg_pgid = (sig_atomic_t)pgid;

    // wait for the foreground job to exit or stop
    bool stopped = false;
    int rc = wait_for_foreground_job(pgid, pids, stopped);

    // clear foreground pgid and reclaim the terminal
    g_fg_pgid = 0;
    reclaim_terminal();

    // if stopped, add to the job table as stopped and print
    if (stopped)
    {
        int job_id = add_job(pgid, pids, cmd_display, JobState::STOPPED);
        char mark = job_marker(job_id);
        std::cout << "[" << job_id << "]" << mark << " Stopped  " << cmd_display << "\n";
        return 0;
    }

    return rc;
}

// none of these are used anymore
// build argv array
std::vector<char *> build_argv(const std::vector<std::string> &tokens)
{
    std::vector<char *> argv;
    // for each token add it to argv
    for (const std::string &s : tokens)
    {
        argv.push_back(const_cast<char *>(s.c_str()));
    }
    // null terminate argv
    argv.push_back(nullptr);
    return argv;
}

// runs an external command
int run_external(const std::vector<std::string> &tokens)
{
    // makes sure tokens is not empty
    if (tokens.empty())
    {
        return 0;
    }
    // fork the process to create child
    pid_t pid = fork();

    // if fork fails
    if (pid < 0)
    {
        std::cerr << "fork: " << std::strerror(errno) << "\n";
        return 1;
    }
    // if we are in child process
    if (pid == 0)
    {
        // build argv and call execvp with it
        std::vector<char *> argv = build_argv(tokens);
        execvp(argv[0], argv.data());
        // if we get here then execvp failed
        std::cerr << tokens[0] << ": " << std::strerror(errno) << "\n";
        // 127 means command not found
        _exit(127);
    }

    int status = 0;
    // try until child exits
    while (true)
    {
        // wait for child to return
        pid_t w = waitpid(pid, &status, 0);
        // if interrupted by a signal then retry
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << "waitpid: " << std::strerror(errno) << "\n";
            return 1;
        }
        break;
    }
    return 0;
}

// legacy method to split tokens by whitespace
std::vector<std::string> tokenize_whitespace(const std::string &line)
{
    // create string stream for input stream
    std::istringstream iss(line);
    std::vector<std::string> tokens;

    std::string token;
    // extract individual tokens and add to tokens vector, >> skips whitespace
    while (iss >> token)
    {
        tokens.push_back(token);
    }

    return tokens;
}