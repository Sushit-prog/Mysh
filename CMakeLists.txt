cmake_minimum_required(VERSION 3.20)
project(mysh C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Compiler flags ────────────────────────────────────────────────────────────
set(CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
     -Wstrict-prototypes -Wmissing-prototypes -Wconversion")
set(CMAKE_C_FLAGS_DEBUG
    "-g3 -O0 -fsanitize=undefined -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_RELEASE
    "-O2 -DNDEBUG -D_FORTIFY_SOURCE=2 -fstack-protector-strong")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(include)

# ── Sources ───────────────────────────────────────────────────────────────────
add_executable(mysh
    src/main.c
    src/init/config.c
    src/lexer/lexer.c
    src/parser/parser.c
    src/executor/exec.c
)

# ── Install ───────────────────────────────────────────────────────────────────
# (readline added in Phase 6)