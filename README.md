# myshell

A Unix-like shell implemented in C++, developed incrementally with a focus on core
systems programming concepts: process creation, program execution, pipes, file
descriptor management, and POSIX job control. The project is built and tested on
Linux via WSL and is being extended toward a feature-complete “mini-bash” with
correct foreground/background semantics and interactive signal handling.

## Current Status

The shell currently supports:
- Interactive read–eval–print loop (REPL)
- Operator-aware tokenization for: `|`, `<`, `>`, `>>`, `&`
- Parsing into a structured command model:
  - pipeline stages
  - per-stage redirection metadata
  - background flag (`&`)
- Builtin commands:
  - `cd`, `exit`
  - `jobs`, `fg`, `bg`
- External command execution using `fork()`, `execvp()`, and `waitpid()`
- I/O redirection using `open()` + `dup2()`:
  - stdin redirection: `<`
  - stdout redirection: `>`
  - stdout append: `>>`
- Pipelines of arbitrary length using `pipe()` + `dup2()`
- Background jobs with a job table and asynchronous reaping (`SIGCHLD`, `WNOHANG`)
- Process groups and terminal control (`setpgid`, `tcsetpgrp`)
- Interactive signal behavior:
  - `Ctrl-C` (SIGINT) and `Ctrl-Z` (SIGTSTP) forwarded to the foreground job
  - the shell stays alive and regains terminal ownership correctly

---

## Features (Implemented)

### Interactive Shell
- Displays a prompt and reads user input line-by-line
- Exits on EOF (`Ctrl-D`) or `exit`

### Tokenization + Parsing
- Recognizes operators as separate tokens (including `>>` as a single token)
- Parses command lines into a structured model:
  - `CommandLine.pipeline` (vector of `Command`)
  - redirection fields per stage
  - `CommandLine.background` for `&`
- Detects and reports common syntax errors (missing command, missing redirection filename, etc.)

### Redirection
- Executes commands with I/O redirection:
  - `cmd < input.txt`
  - `cmd > output.txt`
  - `cmd >> output.txt`
- Implements redirection in the child process before `execvp()` using `open()` and `dup2()`

### Pipelines
- Supports pipelines of arbitrary length:
  - `a | b | c`
- Connects stdout of stage `i` to stdin of stage `i+1` using `pipe()` and `dup2()`
- Ensures correct closing of unused pipe ends in both parent and child

### Background Jobs + Job Table
- Supports background execution using `&`:
  - `sleep 5 &`
- Stores background/stopped jobs in a job table with:
  - job id (`[1]`, `[2]`, ...)
  - process group id (pgid)
  - command string
  - state: Running / Stopped
- Asynchronously reaps finished jobs via `SIGCHLD` + `waitpid(WNOHANG)` and prints completion notifications

### Job Control (`jobs`, `fg`, `bg`)
- `jobs` prints current job table with `+` (current) and `-` (previous) markers
- `fg [%job]`:
  - gives terminal control to the job’s process group
  - resumes the job (`SIGCONT`) if needed
  - waits in the foreground until the job stops or finishes
- `bg [%job]`:
  - resumes a stopped job in the background (`SIGCONT`)
  - keeps the shell interactive

### Terminal Control + Signals
- The shell runs in its own process group and owns the terminal
- Foreground jobs are placed into their own process group and temporarily receive terminal ownership
- `SIGINT` and `SIGTSTP` are forwarded to the current foreground process group
- `SIGCHLD` triggers asynchronous reaping so background job completion is reported without requiring another command

### Build & Run
make
./myshell