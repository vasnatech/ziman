CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -g
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := ziman

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	./tests/run_tests.sh

clean:
	rm -f $(BIN) src/*.o
