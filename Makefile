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

# Bundled core modules (see src/corelib.c): each corelib/<name>.zm file
# is embedded as a C string constant, generated fresh from its .zm
# source every build so the two never drift. Declared as a prerequisite
# of every object file (not just corelib.o) so a from-clean build always
# generates it before anything tries to #include it -- the normal
# -MMD/-MP header tracking only kicks in from the second build onward,
# once a .d file already mentions it. Add a new core module by adding
# its name here and one line to corelib.c's CORE_MODULES table.
CORELIB_MODULES := collections io
CORELIB_HEADERS := $(CORELIB_MODULES:%=src/%_source.h)

$(OBJ): $(CORELIB_HEADERS)

# corelib/<name>.zm -> src/<name>_source.h, a
# `#define <NAME>_SOURCE "..."` (module name uppercased). A #define, not
# a `static const char *`, so it's usable as a constant initializer in
# corelib.c's CORE_MODULES table -- a plain variable can't be (ISO C
# requires a static initializer to itself be a constant expression). One
# quoted string literal per source line, backslash-newline-continued
# across physical lines (same shape prelude.c's hand-written
# PRELUDE_SOURCE uses) rather than one unreadable giant line -- adjacent
# string literals concatenate into a single one regardless of how many
# physical lines or trailing "" the macro's definition spans.
src/%_source.h: corelib/%.zm
	awk -v mod="$*" -v macro="$(shell echo $* | tr a-z A-Z)_SOURCE" \
	    'BEGIN { printf "/* Generated from corelib/%s.zm -- do not edit directly. */\n#define %s \\\n", mod, macro } \
	     { gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); printf "    \"%s\\n\" \\\n", $$0 } \
	     END { print "    \"\"" }' $< > $@

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
	rm -f $(BIN) src/*.o src/*.d src/*_source.h
