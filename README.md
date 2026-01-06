# myshell

A Unix-like shell implemented in C++, developed incrementally with a focus on
systems programming fundamentals: process creation, program execution, I/O
redirection, pipelines, and Unix job control. The project is built and tested
on Linux using WSL and is structured to grow into a feature-complete “mini-bash”
with correct process group and terminal semantics.

## Current Status

**Update 8 complete (fg/bg job control builtins)**

The shell currently supports:
- An interactive read–eval–print loop
- Operator-aware tokenization for: `|`, `<`, `>`, `>>`, `&`
- Parsing into a structured command model (pipeline stages + redirection metadata + background flag)
- Builtin commands: `cd`, `exit`, `jobs`, `fg`, `bg`
- Execution of external commands using `fork`, `execvp`, and `waitpid`
- Input/output redirection for single commands and pipelines using `open` + `dup2`:
  - stdin redirection: `<`
  - stdout redirection: `>`
  - stdout append: `>>`
- Pipelines of arbitrary length using `pipe` + `dup2`
- Background execution (`&`) with a job table
- Process groups and terminal control (`setpgid`, `tcsetpgrp`) for correct interactive behavior
- Signal handling:
  - `SIGCHLD` for asynchronous child reaping (background completion notices appear without new input)
  - `SIGINT` / `SIGTSTP` forwarded to the foreground process group (Ctrl-C / Ctrl-Z)
- Foreground/background job control:
  - `fg` brings a job to the foreground and hands it the terminal
  - `bg` resumes a stopped job in the background

## Features (Implemented)

### Interactive Shell
- Displays a prompt and reads user input line-by-line
- Exits cleanly on EOF (`Ctrl-D`) or `exit`

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
- Reports file/permission errors cleanly and returns to the prompt

### Pipelines
- Executes pipelines of arbitrary length:
  - `a | b | c`
- Connects stages using `pipe()` and `dup2()`:
  - previous stage stdout → next stage stdin
- Supports redirection with pipelines:
  - input redirection allowed only on the first stage
  - output redirection allowed only on the last stage

### Background Jobs + Job Table
- Runs commands/pipelines in the background with `&`
- Stores jobs in a job table with:
  - job id
  - process group id (pgid)
  - tracked pids (for pipelines)
  - state (RUNNING/STOPPED/DONE)
- `jobs` prints job status

### Job Control (`fg` / `bg`)
- `fg [%job]`:
  - transfers terminal control to the job’s process group (`tcsetpgrp`)
  - resumes the job with `SIGCONT` (if stopped)
  - waits until job stops or completes
- `bg [%job]`:
  - resumes a stopped job in the background using `SIGCONT`
  - does not take terminal control
- Job id formats supported:
  - `fg %1`, `fg 1`, `bg %2`, `bg 2`
- Default behavior:
  - `fg` with no args selects the most recent STOPPED job (or most recent RUNNING job if none stopped)
  - `bg` with no args selects the most recent STOPPED job

### Signal Handling
- `SIGCHLD` triggers non-blocking reaping (`waitpid` with `WNOHANG|WUNTRACED|WCONTINUED`)
- Background job completion messages appear asynchronously (no need to run a new command)
- Ctrl-C (`SIGINT`) and Ctrl-Z (`SIGTSTP`) are forwarded to the foreground process group

