# mysh

A POSIX-compliant shell written in C11 from scratch — built for correctness, security, and extensibility.

![Version](https://img.shields.io/badge/version-1.0.0-blue)
![Language](https://img.shields.io/badge/language-C11-lightgrey)
![Build](https://img.shields.io/badge/build-CMake-green)
![License](https://img.shields.io/badge/license-MIT-orange)

---

## Overview

mysh is a Unix shell built from the ground up in C11, implementing a full pipeline from lexical analysis through AST construction to process execution. It was designed with an emphasis on POSIX compliance, memory safety, and defence against the vulnerability classes that affect real-world shells — fd exhaustion, glob injection, signal races, symlink attacks, and PATH hijacking.

This is not a tutorial shell. It implements a real recursive descent parser that produces a typed AST, a multi-stage word expansion engine in the correct POSIX order, a POSIX job table with terminal ownership transfer, and persistent line editing via linenoise.

---

## Features

### Core shell
- Interactive REPL with linenoise line editing — arrow keys, Ctrl-R history search, Emacs keybindings
- Persistent command history saved to `~/.mysh_history`
- `~/.myshrc` loaded at startup for aliases, exports, and PS1 customisation
- PS1 prompt with automatic `~` substitution for `$HOME`

### Parser and execution
- Recursive descent parser producing a typed AST
- Node types: `NODE_CMD`, `NODE_PIPE`, `NODE_AND`, `NODE_OR`, `NODE_SEQ`, `NODE_BACKGROUND`, `NODE_SUBSHELL`
- Pipelines with correct fd wiring — all pipes created before any fork, `O_CLOEXEC` set on every end
- Short-circuit evaluation for `&&` and `||`
- Sequential execution with `;`
- Background jobs with `&`
- Subshell execution with `( )`

### Word expansion (POSIX order)
- Tilde expansion (`~`, `~/path`)
- Parameter expansion — `$VAR`, `${VAR}`, `$?`, `$$`, `$#`
- Command substitution — `$(command)`
- Pathname expansion (globbing) — `*`, `?`, `[...]` with a 256-match safety cap
- Adjacent quoted/unquoted segments joined into a single token

### Redirections
- Input `<`, output `>`, append `>>` — all with `O_CLOEXEC` and `O_NOFOLLOW`
- Heredoc `<<DELIM`

### Job control
- Full POSIX job table with process group tracking
- `SIGCHLD` handler with `waitpid(WNOHANG | WUNTRACED)` loop
- Terminal ownership transfer via `tcsetpgrp` on `fg`
- Completed jobs reported before each prompt (never from inside a signal handler)

### Builtins
| Builtin | Description |
|---------|-------------|
| `cd` | Change directory, updates `$PWD` |
| `pwd` | Print working directory |
| `exit [n]` | Exit with optional status code |
| `export [NAME=VAL]` | Set and export environment variables |
| `unset NAME` | Remove environment variable |
| `alias [NAME=VAL]` | Define or list aliases |
| `unalias NAME` | Remove an alias |
| `source FILE` / `. FILE` | Execute a file in the current shell |
| `type NAME` | Show whether a name is a builtin, alias, or external command |
| `jobs` | List background and stopped jobs |
| `fg [%N]` | Bring job to foreground |
| `bg [%N]` | Resume stopped job in background |
| `kill [-SIG] %N\|PID` | Send signal to job or process |

---

## Architecture

```
src/
├── main.c                  Entry point
├── init/
│   └── config.c            REPL loop, signal setup, builtin dispatch, .myshrc loader
├── lexer/
│   └── lexer.c             State-machine tokeniser — quotes, escapes, operators, $() inline
├── parser/
│   └── parser.c            Recursive descent parser → typed AST
├── expand/
│   └── expand.c            Multi-stage POSIX word expansion engine
├── executor/
│   └── exec.c              AST walker, pipeline wiring, process management
├── jobs/
│   └── jobs.c              POSIX job table — add, remove, find, report
└── linenoise/
    └── linenoise.c         Single-file readline replacement (antirez/linenoise)

include/
├── mysh.h                  Global state, job table, ShellState, alias table
├── lexer.h                 Token types and lexer API
├── parser.h                AST node types and parser API
├── executor.h              Command and redirection types, exec API
└── expand.h                Word expansion API
```

The key architectural decision is a strict separation between the three phases: the lexer produces a token stream, the parser consumes it into a pure AST data structure with no side effects, and the executor walks that AST. No parsing happens during execution and no execution happens during parsing.

---

## Security

Every design decision was made with a specific vulnerability class in mind.

| Threat | Mitigation |
|--------|------------|
| fd leaks into child processes | `O_CLOEXEC` on every `open()` and `pipe()` call |
| Symlink attacks on output redirection | `O_NOFOLLOW` on every `O_WRONLY` open |
| Glob injection / DoS | Expansion happens after parse; glob results capped at 256 |
| Signal races | `sigaction()` with `SA_RESTART` throughout; never bare `signal()` |
| Async-signal-safety violations | Signal handlers only set `volatile sig_atomic_t` flags; all work done in the main loop |
| Buffer overflows on input | Fully dynamic input buffers — no static arrays for user input |
| Undefined behaviour | UBSan (`-fsanitize=undefined`) enabled in every Debug build |
| Memory corruption | `_FORTIFY_SOURCE=2` and `-fstack-protector-strong` in Release builds |
| PATH hijacking | Relative PATH components warned at startup |
| Double-flush / atexit in child | `_exit()` in every child process, never `exit()` |

---

## Build

**Requirements:** GCC or Clang, CMake ≥ 3.20, Linux or WSL2

```bash
git clone https://github.com/Sushit-prog/Mysh.git
cd Mysh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
./mysh
```

For a Release build with full hardening:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

> **WSL2 note:** ASan's shadow memory reservation fails on WSL2 kernels. The Debug build uses UBSan only (`-fsanitize=undefined`). Use Valgrind (`valgrind --leak-check=full ./mysh`) for periodic memory checks.

---

## Configuration

On startup, mysh loads `~/.myshrc` if it exists. Example:

```sh
# ~/.myshrc

export EDITOR=nvim
export PS1='mysh $PWD > '

alias ll='ls -la'
alias gs='git status'
alias gp='git push'
alias ..='cd ..'
```

---

## Usage Examples

```sh
# Pipelines
ls -la | grep mysh | wc -l

# Redirections
echo "hello" > out.txt
cat >> out.txt << EOF
line two
line three
EOF

# Logical operators
make && echo "build ok" || echo "build failed"

# Background jobs
sleep 30 &
jobs
fg %1

# Variable expansion
export NAME=world
echo "hello $NAME"
echo "pid is $$"
echo "last exit: $?"

# Command substitution
echo "today is $(date)"
cd $(git rev-parse --show-toplevel)

# Glob expansion
ls *.c
rm build/*.o

# Aliases
alias gc='git commit -m'
gc "feat: add feature"
```

---

## Roadmap

- [ ] Arithmetic expansion `$(( ))`
- [ ] `${VAR:-default}` / `${VAR:=default}` parameter modifiers
- [ ] IFS-aware word splitting
- [ ] Here-string `<<<`
- [ ] Tab completion via linenoise completion API
- [ ] `history` builtin
- [ ] AFL++ fuzz harness on lexer, parser, and expansion engine
- [ ] seccomp sandbox for child processes

---

## References

The following resources informed the architecture and security decisions:

- [Writing a Unix Shell](https://indradhanush.github.io/blog/writing-a-unix-shell-part-1/) — Indradhanush Gupta
- [Build Your Own Shell](https://github.com/tokenrove/build-your-own-shell) — tokenrove
- [Write a Shell in C](https://brennan.io/2015/01/16/write-a-shell-in-c/) — Stephen Brennan
- [Shell Workshop](https://github.com/kamalmarhubi/shell-workshop) — Kamal Marhubi
- [linenoise](https://github.com/antirez/linenoise) — antirez (line editing, MIT licensed)
- POSIX.1-2017 Shell and Utilities specification

---

## License

MIT
