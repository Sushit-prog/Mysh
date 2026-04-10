# 🐚 MySH — A Custom Unix Shell Implementation

**MySH** is a custom Unix-like shell written in C, designed to explore how command-line interpreters work internally.
This project focuses on understanding **process management, parsing, pipes, redirection, and signals** by implementing a modular shell architecture.

---

# 📌 Project Overview

Building a shell provides deep insight into:

* Process creation and management
* File descriptor manipulation
* Command parsing and execution
* Unix signals and job control
* Environment handling

This project follows a **layered architecture**, separating user input handling, parsing logic, and execution.

---

# 🏗️ Architecture

The shell follows a modular pipeline similar to modern command-line interpreters.

```
User Input
    │
    ▼
User Interface Layer
    │
    ▼
Lexer / Tokenizer
    │
    ▼
Parser (AST Builder)
    │
    ▼
Expansion Engine
    │
    ▼
Executor
    │
    ▼
I/O Redirection & Pipes
    │
    ▼
System Calls
```

---

# ⚙️ Features Implemented (So Far)

## ✅ Core Execution

* Command input loop
* Basic command execution
* Forking child processes
* Executing programs using `execve()`
* Waiting for process completion

## ✅ Lexer / Tokenizer

* Splits input into tokens
* Recognizes:

  * Commands
  * Arguments
  * Pipes (`|`)
  * Redirections (`>`, `<`)
  * Strings with quotes

Example:

```
Input:
ls -l | grep "txt"

Tokens:
[WORD(ls), WORD(-l), PIPE(|), WORD(grep), STRING(txt)]
```

## 🚧 Parser (In Progress)

* Building Abstract Syntax Tree (AST)
* Identifying command relationships
* Supporting pipelines

## 🚧 Execution Pipeline

* Planned support for:

  * Pipes (`|`)
  * Output redirection (`>`)
  * Input redirection (`<`)

---

# 🧠 Core Concepts Used

This project heavily uses core **Unix/Linux system calls**.

| Category           | Functions Used            |
| ------------------ | ------------------------- |
| Process Management | `fork()`, `waitpid()`     |
| Program Execution  | `execve()`                |
| File Handling      | `open()`, `close()`       |
| Pipes              | `pipe()`                  |
| Redirection        | `dup2()`                  |
| Signals            | `sigaction()` *(planned)* |

---

# 📂 Project Structure

```
mysh/
│
├── src/
│   ├── main.c
│   ├── lexer.c
│   ├── parser.c
│   ├── executor.c
│   ├── io.c
│   └── security.c
│
├── include/
│   ├── lexer.h
│   ├── parser.h
│   ├── executor.h
│   └── security.h
│
├── Makefile
├── README.md
├── .clang-format
└── .clang-tidy
```

---

# 🚀 Getting Started

## Prerequisites

* Linux or WSL
* GCC Compiler
* Make

Install dependencies:

```bash
sudo apt update
sudo apt install build-essential
```

---

## Build

```bash
make
```

---

## Run

```bash
./mysh
```

---

# 🧪 Example Usage

```
$ ./mysh

mysh> ls
file1.txt
file2.c

mysh> echo Hello World
Hello World
```

---

# 🛠️ Development Roadmap

## Phase 1 — Core Shell Loop ✅

* [x] Input reading
* [x] Basic command execution
* [x] Process management

## Phase 2 — Tokenization ✅

* [x] Word splitting
* [x] Pipe detection
* [x] Quote handling

## Phase 3 — Parsing 🚧

* [ ] AST creation
* [ ] Pipeline parsing

## Phase 4 — Redirection 🚧

* [ ] Output redirection (`>`)
* [ ] Input redirection (`<`)

## Phase 5 — Pipes 🚧

* [ ] Multi-command pipelines

## Phase 6 — Builtins 🔜

* [ ] `cd`
* [ ] `exit`
* [ ] `export`

## Phase 7 — Signals 🔜

* [ ] Handle `Ctrl+C`
* [ ] Job control

---

# 🧩 Built-in Commands (Planned)

These commands must be implemented internally:

* `cd`
* `exit`
* `pwd`
* `export`
* `unset`
* `env`

---

# 📖 Learning Goals

This project is designed to strengthen understanding of:

* Operating System fundamentals
* Unix system calls
* Process lifecycle
* File descriptors
* Parsing techniques
* Memory management in C

---

# 🐞 Debugging Tips

Helpful tools during development:

```bash
strace ./mysh
valgrind ./mysh
gdb ./mysh
```

---

# 🤝 Contributing

Contributions are welcome.

Steps:

```bash
fork repository
create feature branch
commit changes
open pull request
```

---

# 📜 License

This project is released under the MIT License.

---

# 🙌 Acknowledgements

Inspired by:

* Unix Shell Design
* POSIX Standards
* Classic Shell Implementations
* System Programming Concepts

---

# ⭐ Why This Project Matters

Writing a shell is one of the most powerful ways to understand:

* How programs start
* How processes communicate
* How operating systems interact with user commands

It transforms system programming theory into real-world execution.

---
