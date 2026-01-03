# myshell

A Unix-like shell implemented in C++, developed incrementally with a focus on
systems programming fundamentals: process creation, program execution, and
event-loop design. The project is built and tested on Linux using WSL and is
structured to grow into a feature-complete “mini-bash” with pipelines, job
control, and signal handling.

## Current Status

**Update 4 complete (pipelines + end redirection)**

The shell currently supports:
- An interactive read–eval–print loop
- Operator-aware tokenization for: `|`, `<`, `>`, `>>`, `&`
- Parsing into a structured command model (pipeline stages + redirection metadata + background flag)
- Builtin commands (`cd`, `exit`) for simple commands
- Execution of external commands using `fork`, `execvp`, and `waitpid`
- Input/output redirection using `open` + `dup2`:
  - stdin redirection: `<`
  - stdout redirection: `>`
  - stdout append: `>>`
- Pipelines of arbitrary length using `pipe` + `dup2`:
  - `a | b`
  - `a | b | c`
- Redirection at pipeline ends (simplified rules):
  - `<` allowed only on the first stage
  - `>` / `>>` allowed only on the last stage
- Clean build system using `make`

> Note: Background execution (`&`) is parsed but not executed yet.
> Full job control (process groups, terminal control, signals, `jobs/fg/bg`) will be implemented later on

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

### Builtins
- `cd [dir]`
  - Changes the current working directory
  - Defaults to `$HOME` when no directory is provided
- `exit`
  - Terminates the shell cleanly

### External Commands
- Uses `fork()` to create a child process
- Replaces the child process image with `execvp()`
- Parent waits for completion using `waitpid()`
- Proper error reporting on failed execution


