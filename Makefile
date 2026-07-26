CC := gcc
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O2 -pthread
LDFLAGS := -pthread
SRC := src/main.c src/package.c src/fuzzy.c src/pacman_source.c src/aur_source.c src/tui.c
BIN := lazy-search

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN)
