# myshell

A Unix-like shell implemented in C++, developed incrementally with a focus on
systems programming fundamentals: process creation, program execution, and
event-loop design. The project is built and tested on Linux using WSL and is
structured to grow into a feature-complete “mini-bash” with pipelines, job
control, and signal handling.

## Current Status

**Update 3 complete (I/O redirection execution)**

The shell currently supports:
- An interactive read–eval–print loop
- Operator-aware tokenization for: `|`, `<`, `>`, `>>`, `&`
- Parsing into a structured command model (pipeline stages + redirection metadata + background flag)
- Builtin commands (`cd`, `exit`)
- Execution of external commands using `fork`, `execvp`, and `waitpid`
- Input/output redirection for a single command using `open` + `dup2`:
  - stdin redirection: `<`
  - stdout redirection: `>`
  - stdout append: `>>`
- Clean build system using `make`

> Note: Pipelines (`|`) and background execution (`&`) are parsed but not executed yet.
> These will be implemented soon.

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


