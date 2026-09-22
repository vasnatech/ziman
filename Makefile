CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -g -MMD -MP -pthread
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)
BIN := ziman

.PHONY: all clean test test-bignum test-decimal

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lm -pthread

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEP)

test: $(BIN)
	./tests/run_tests.sh

# Unit tests for internal C modules that don't fit the language-level
# tests/fixtures harness (e.g. bignum.c's own arithmetic correctness,
# independent of any Ziman-level syntax). Each has its own main(), so it
# lives outside src/ (which this Makefile wildcards into the ziman
# binary) and is built/run standalone rather than joining $(OBJ).
test-bignum: src/bignum.c tests/unit/bignum_test.c
	$(CC) $(CFLAGS) -o /tmp/ziman_bignum_test src/bignum.c tests/unit/bignum_test.c
	/tmp/ziman_bignum_test

test-decimal: src/bignum.c src/decimal.c tests/unit/decimal_test.c
	$(CC) $(CFLAGS) -o /tmp/ziman_decimal_test src/bignum.c src/decimal.c tests/unit/decimal_test.c
	/tmp/ziman_decimal_test

clean:
	rm -f $(BIN) src/*.o src/*.d
