# myshell

A Unix-like shell implemented in C++, developed incrementally with a focus on
systems programming fundamentals: process creation, program execution, pipelines,
and Unix job control. The project is built and tested on Linux using WSL and is
structured to grow into a feature-complete “mini-bash.”

## Current Status

**Update 7 complete (signals + asynchronous child reaping)**

The shell currently supports:
- An interactive read–eval–print loop
- Operator-aware tokenization for: `|`, `<`, `>`, `>>`, `&`
- Parsing into a structured command model (pipeline stages + redirection metadata + background flag)
- Builtin commands (`cd`, `exit`, `jobs`)
- Execution of external commands using `fork`, `execvp`, and `waitpid`
- Input/output redirection using `open` + `dup2`
- Pipelines of arbitrary length using `pipe` + `dup2`
- Background execution for both single commands and pipelines
- Per-job process groups with terminal control for foreground jobs (`setpgid`, `tcsetpgrp`)
- Signal handling for interactive behavior:
  - `SIGCHLD` triggers asynchronous child reaping
  - `SIGINT` (Ctrl-C) forwards to the foreground process group
  - `SIGTSTP` (Ctrl-Z) forwards to the foreground process group
- Job table with basic state tracking (`Running`, `Stopped`, `Done`)
- Clean build system using `make`

> Note: This update focuses on correct signal delivery and prompt responsiveness.
> Full `fg/bg` job control semantics are planned for the next milestone.

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
- Executes commands with I/O redirection:
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
- Connects stages using `dup2()` and closes unused pipe fds to ensure correct EOF propagation
- Supports end redirection on pipelines under simplified rules:
  - `< file` only on the first stage
  - `> file` / `>> file` only on the last stage

### Process Groups + Terminal Control
- Each command/pipeline runs in its own process group (PGID)
- Foreground jobs:
  - receive terminal control via `tcsetpgrp()`
  - return terminal control to the shell upon completion/stop
- Background jobs:
  - run in their own process group
  - do not receive terminal control

### Signals + Asynchronous Reaping
- Uses `sigaction()` to install signal handlers with predictable behavior
- `SIGCHLD`:
  - triggers non-blocking reaping with `waitpid(..., WNOHANG | WUNTRACED | WCONTINUED)`
  - allows background completion messages to appear promptly (without waiting for the next command)
- `SIGINT` (Ctrl-C):
  - forwarded to the foreground process group so the running job is interrupted, not the shell
- `SIGTSTP` (Ctrl-Z):
  - forwarded to the foreground process group so the job stops and returns control to the shell

### Background Jobs + Job Table
- Supports background execution with `&`:
  - `sleep 2 &`
  - `cmd1 | cmd2 &`
- Tracks jobs with:
  - job id
  - pgid
  - pids
  - command string
  - state (`Running`, `Stopped`, `Done`)
- `jobs` builtin lists the current job table

