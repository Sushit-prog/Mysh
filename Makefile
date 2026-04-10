CC      = gcc
CFLAGS  = -std=c17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -g -Isrc
LDFLAGS = -fsanitize=address,undefined

SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:.c=.o)
TARGET  = mysh

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $

clean:
	rm -f src/*.o $(TARGET)

.PHONY: all clean
