# myshell

A Unix-like shell implemented in C++, developed incrementally with a focus on
systems programming fundamentals: process creation, program execution, and
event-loop design. The project is built and tested on Linux using WSL and is
structured to grow into a feature-complete “mini-bash” with pipelines, job
control, and signal handling.

## Current Status

The shell currently supports:
- An interactive read–eval–print loop
- Whitespace-based tokenization
- Builtin commands (`cd`, `exit`)
- Execution of external programs using `fork`, `execvp`, and `waitpid`
- Clean build system using `make`

## Features (Implemented)

### Interactive Shell
- Displays a prompt and reads user input line-by-line
- Gracefully exits on EOF (`Ctrl-D`) or `exit`

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

---

## Future updates

The following milestones are planned and tracked explicitly:

- **Update 2**: Operator-aware tokenization  
  (`|`, `<`, `>`, `>>`, `&`)
- **Update 3**: Input/output redirection
- **Update 4**: Pipelines with arbitrary length
- **Update 5**: Background jobs and job table
- **Update 6**: Process groups and terminal control
- **Update 7**: Signal handling (`SIGCHLD`, `SIGINT`, `SIGTSTP`)
- **Update 8–9**: Full job control (`jobs`, `fg`, `bg`)

The final goal is a shell that behaves correctly with respect to Unix job
control semantics.
