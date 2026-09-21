CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -g -MMD -MP -pthread
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)
BIN := ziman

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lm -pthread

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEP)

test: $(BIN)
	./tests/run_tests.sh

clean:
	rm -f $(BIN) src/*.o src/*.d
