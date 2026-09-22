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

# Bundled core modules (see src/corelib.c): each corelib/*.zm file is
# embedded as a C string constant, generated fresh from its .zm source
# every build so the two never drift. Declared as a prerequisite of
# every object file (not just corelib.o) so a from-clean build always
# generates it before anything tries to #include it -- the normal
# -MMD/-MP header tracking only kicks in from the second build onward,
# once a .d file already mentions it.
$(OBJ): src/collections_source.h

# A #define, not a `static const char *`, so it's usable as a constant
# initializer in corelib.c's CORE_MODULES table -- a plain variable
# can't be (ISO C requires a static initializer to itself be a constant
# expression). Emitted as one long line rather than one string literal
# per source line to avoid needing backslash-newline continuations in a
# macro definition.
src/collections_source.h: corelib/collections.zm
	awk 'BEGIN { printf "/* Generated from corelib/collections.zm -- do not edit directly. */\n#define COLLECTIONS_SOURCE \"" } \
	     { gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); printf "%s\\n", $$0 } \
	     END { print "\"" }' corelib/collections.zm > $@

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
	rm -f $(BIN) src/*.o src/*.d src/collections_source.h
