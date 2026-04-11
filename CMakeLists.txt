cmake_minimum_required(VERSION 3.20)
project(mysh C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

include_directories(include)

add_executable(mysh
    src/main.c
    src/init/config.c
    src/lexer/lexer.c
    src/parser/parser.c
    src/expand/expand.c
    src/executor/exec.c
    src/jobs/jobs.c
)
