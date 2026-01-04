# myshell

A Unix-like shell implemented in C++, developed incrementally with a focus on
systems programming fundamentals: process creation, program execution, and
event-loop design. The project is built and tested on Linux using WSL and is
structured to grow into a feature-complete “mini-bash” with pipelines, job
control, and signal handling.

## Current Status

**Update 5 complete (background jobs + job table + reaping)**

The shell currently supports:
- An interactive read–eval–print loop
- Operator-aware tokenization for: `|`, `<`, `>`, `>>`, `&`
- Parsing into a structured command model (pipeline stages + redirection metadata + background flag)
- Builtin commands (`cd`, `exit`, `jobs`)
- Execution of external commands using `fork`, `execvp`, and `waitpid`
- Input/output redirection using `open` + `dup2`:
  - stdin redirection: `<`
  - stdout redirection: `>`
  - stdout append: `>>`
- Pipelines of arbitrary length using `pipe` + `dup2`
- Background execution with `&` for both single commands and pipelines
- A simple job table tracking background jobs (job id, pids, command string, running/done)
- Non-blocking child reaping using `waitpid(..., WNOHANG)` to prevent zombie processes
- Clean build system using `make`

> Note: This update implements basic background launching and tracking.
> Full job control semantics (process groups, terminal control, signal routing, and `fg/bg`) are planned for later milestones.

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
- Reports file/permission errors cleanly and returns to the prompt

### Pipeline Execution
- Executes pipelines with arbitrary length:
  - `cmd1 | cmd2`
  - `cmd1 | cmd2 | cmd3`
- Creates `N-1` pipes for `N` stages and forks one process per stage
- Connects stages using `dup2()`:
  - stage `i` stdout → pipe write end
  - stage `i+1` stdin → pipe read end
- Closes unused pipe file descriptors in both parent and child processes to prevent hangs and ensure EOF propagates correctly
- Supports end redirection on pipelines under simplified rules:
  - `< file` only on the first stage
  - `> file` / `>> file` only on the last stage

### Background Jobs + Job Table
- Supports background execution with `&`:
  - `sleep 5 &`
  - `cmd1 | cmd2 &`
- Does not block the shell prompt for background jobs
- Maintains a job table tracking:
  - job id (`[1]`, `[2]`, ...)
  - process ids (single pid for commands, multiple pids for pipelines)
  - original command string (best-effort)
  - job state (`Running` / `Done`)
- Reaps completed background children using `waitpid(-1, ..., WNOHANG)` to prevent zombie processes
- Prints a completion notification when a job finishes
- Includes a `jobs` builtin to list current jobs

### Builtins
- `cd [dir]`
  - Changes the current working directory
  - Defaults to `$HOME` when no directory is provided
- `exit`
  - Terminates the shell cleanly
- `jobs`
  - Lists tracked background jobs and their status

### External Commands
- Uses `fork()` to create a child process
- Replaces the child process image with `execvp()`
- Parent waits for completion using `waitpid()` for foreground jobs
- Proper error reporting on failed execution
