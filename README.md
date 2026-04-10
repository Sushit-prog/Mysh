Custom Shell Implementation
A high-performance, POSIX-aligned command-line interpreter built from the ground up. This project implements the core logic of a modern shell, including a full recursive descent parser, process lifecycle management, and complex I/O redirection.

1. Project Architecture
The shell is engineered following a modular pipeline architecture to ensure separation of concerns and extensibility:

User Interface Layer: Managed via readline/libedit for robust line editing, history persistence, and Unicode support.

Lexer & Tokenizer: A state-machine-based lexer that handles quoting, escaping, and operator identification.

Parser (AST): Converts tokens into an Abstract Syntax Tree (AST) to support complex command structures like pipelines and compound commands.

Expansion Engine: Handles environmental variable resolution ($VAR), tilde expansion (~), and globbing.

Execution Core: Manages the fork/exec lifecycle, process synchronization, and exit status tracking.

2. Features implemented
Core Execution
[x] Simple command execution via execve.

[x] Path resolution and error handling (command not found).

[x] Support for command arguments and environment inheritance.

I/O & Redirection
[x] Input redirection (<) and Output redirection (>, >>).

[x] Pipe implementation (|) for inter-process communication.

[x] File descriptor management using dup2.

Built-in Commands
[x] cd: Directory navigation.

[x] export / unset: Environment variable management.

[x] alias: Command shortcutting.

[x] exit: Graceful shell termination.

3. Technical Implementation Details
The AST Pipeline
The shell doesn't just execute strings; it parses them into a tree structure. This allows for logical execution of commands:

Plaintext
Command: ls -l | grep ".c" > output.txt

       [Redirection: >]
           /      \
      [Pipe: |]  [File: output.txt]
       /     \
    [ls]    [grep]
Signal Handling
Implemented robust signal masks to ensure that Ctrl+C (SIGINT) and Ctrl+\ (SIGQUIT) interact correctly with child processes without terminating the parent shell.

4. Getting Started
Prerequisites
GCC / Clang

Make

Readline library (libreadline-dev on Debian/Ubuntu)

Installation
Clone the repository:

Bash
git clone https://github.com/Sushit-prog/shell-project.git
Build the project:

Bash
make
Run the shell:

Bash
./myshell
5. Future Roadmap
[ ] Job Control: Implementation of fg, bg, and process group management.

[ ] Security Hardening: Integrating seccomp for system call filtering.

[ ] Scripting Support: Handling if/else logic and while loops.

Author
Sushit Lal Pakrashy Full-Stack Developer & GenAI Engineer
