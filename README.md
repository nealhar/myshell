# myshell

A Unix-like shell implemented in C++, developed incrementally with a focus on
systems programming fundamentals: process creation, program execution, and
Unix job control. The project is built and tested on Linux using WSL and is
structured to grow into a feature-complete “mini-bash.”

## Current Status

The shell currently supports:
- An interactive read–eval–print loop
- Operator-aware tokenization for: `|`, `<`, `>`, `>>`, `&`
- Parsing into a structured command model (pipeline stages + redirection metadata + background flag)
- Builtin commands (`cd`, `exit`, `jobs`)
- Execution of external commands using `fork`, `execvp`, and `waitpid`
- Input/output redirection using `open` + `dup2`
- Pipelines of arbitrary length using `pipe` + `dup2`
- Background execution for both single commands and pipelines
- A job table tracking background jobs (job id, pgid, pids, command string, state)
- Foreground job execution using process groups and terminal control (`setpgid`, `tcsetpgrp`)
- Non-blocking reaping of background children using `waitpid(..., WNOHANG)`
- Clean build system using `make`

> Note: Background job completion is detected via polling at the start of each
> shell loop iteration. Job completion messages may appear when the shell
> regains control (e.g., after pressing Enter). Asynchronous signal-driven
> reaping will be implemented in a later update.

---

## Features (Implemented)

### Interactive Shell
- Displays a prompt and reads user input line-by-line
- Gracefully exits on EOF (`Ctrl-D`) or `exit`

### Tokenization + Parsing
- Recognizes operators as separate tokens (including `>>` as one token)
- Parses command lines into a structured model:
  - pipeline of command stages
  - per-stage redirection fields (`<`, `>`, `>>`)
  - trailing background indicator (`&`)
- Detects and reports basic syntax errors (e.g., missing command, missing redirection filename)

### Redirection Execution
- Executes a single parsed command with I/O redirection:
  - `cmd < input.txt`
  - `cmd > output.txt`
  - `cmd >> output.txt`
- Implements redirection in the child process prior to `execvp()`:
  - opens files with `open()`
  - rewires file descriptors with `dup2()`
  - closes original descriptors after duplication

### Pipeline Execution
- Executes pipelines with arbitrary length:
  - `cmd1 | cmd2`
  - `cmd1 | cmd2 | cmd3`
- Creates `N-1` pipes for `N` stages and forks one process per stage
- Connects pipeline stages using `dup2()`:
  - stage `i` stdout → pipe write end
  - stage `i+1` stdin → pipe read end
- Closes unused pipe file descriptors in both parent and child processes to
  ensure correct EOF behavior
- Supports end redirection on pipelines under simplified rules:
  - `< file` only on the first stage
  - `> file` / `>> file` only on the last stage

### Background Jobs
- Supports background execution with `&`:
  - `sleep 5 &`
  - `cmd1 | cmd2 &`
- Shell does not block on background jobs
- Maintains a job table tracking:
  - job id
  - process group id (pgid)
  - process ids
  - original command string
  - running/done state
- Includes a `jobs` builtin to list background jobs

### Process Groups + Terminal Control
- Each command or pipeline runs in its own process group
- Foreground jobs:
  - receive terminal control via `tcsetpgrp()`
  - run isolated from the shell
  - return terminal control to the shell on completion
- Background jobs:
  - run in separate process groups
  - do not receive terminal control
- Shell runs in its own process group and ignores terminal job-control signals
  (`SIGTTOU`, `SIGTTIN`, `SIGTSTP`) to prevent self-stopping

