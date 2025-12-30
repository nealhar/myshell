# myshell

A Unix-like shell implemented in C++, developed incrementally with a focus on
systems programming fundamentals: process creation, program execution, and
event-loop design. The project is built and tested on Linux using WSL and is
structured to grow into a feature-complete “mini-bash” with pipelines, job
control, and signal handling.

## Current Status

**Update 2 complete (operator-aware tokenization + parsing model)**

The shell currently supports:
- An interactive read–eval–print loop
- Operator-aware tokenization for: `|`, `<`, `>`, `>>`, `&`
- Parsing into a structured command model (pipeline stages + redirection metadata + background flag)
- Builtin commands (`cd`, `exit`)
- Execution of simple external commands using `fork`, `execvp`, and `waitpid`
- Clean build system using `make`

> Note: Operator execution (pipes/redirection/background) is parsed but not executed yet.
> Execution will be implemented in the next milestones.

## Features (Implemented)

### Interactive Shell
- Displays a prompt and reads user input line-by-line
- Gracefully exits on EOF (`Ctrl-D`) or `exit`

### Tokenization + Parsing (Update 2)
- Recognizes operators as separate tokens (including `>>` as one token)
- Parses command lines into a structured model:
  - pipeline of command stages
  - per-stage redirection fields (`<`, `>`, `>>`)
  - trailing background indicator (`&`)
- Detects and reports basic syntax errors (e.g., missing command, missing redirection filename)

### Builtins
- `cd [dir]`
  - Changes the current working directory
  - Defaults to `$HOME` when no directory is provided
- `exit`
  - Terminates the shell cleanly

### External Commands (Update 1 behavior)
- Uses `fork()` to create a child process
- Replaces the child process image with `execvp()`
- Parent waits for completion using `waitpid()`
- Proper error reporting on failed execution

---

## Future Updates

The following milestones are planned and tracked explicitly:

- **Update 3**: Input/output redirection execution (`<`, `>`, `>>`)
- **Update 4**: Pipelines with arbitrary length (`a | b | c`)
- **Update 5**: Background jobs (`&`) and job table
- **Update 6**: Process groups and terminal control (`setpgid`, `tcsetpgrp`)
- **Update 7**: Signal handling (`SIGCHLD`, `SIGINT`, `SIGTSTP`)
- **Update 8–9**: Full job control (`jobs`, `fg`, `bg`)

The final goal is a shell that behaves correctly with respect to Unix job
control semantics.
