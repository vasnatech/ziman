CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -g -MMD -MP -pthread
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)
BIN := ziman

.PHONY: all clean test test-pg test-tls test-bignum test-decimal corelib-reference

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lm -lsqlite3 -lssl -lcrypto -pthread

# Bundled core modules (see src/corelib.c): each lib/core/<name>.zm file
# is embedded as a C string constant, generated fresh from its .zm
# source every build so the two never drift. Declared as a prerequisite
# of every object file (not just corelib.o) so a from-clean build always
# generates it before anything tries to #include it -- the normal
# -MMD/-MP header tracking only kicks in from the second build onward,
# once a .d file already mentions it. Add a new core module by adding
# its name here and one line to corelib.c's CORE_MODULES table.
#
# `prelude` is the one exception: it's listed here purely to get its
# header generated the same readable, never-hand-escaped way every other
# lib/core or lib/db module does, but it deliberately has NO matching line in
# corelib.c's CORE_MODULES table -- src/prelude.c reads its generated
# header directly, and it's merged bare into every program by
# loader.c's load_module_graph (see lib/prelude.zm's own header
# comment), never through a `use`. Do not add it to corelib.c's table.
CORELIB_MODULES := collections io random uuid clock time json number sql dburl sqlite db bytes net crypto pg http os properties prelude
# Bundled modules live in lib/core (general-purpose) and lib/db (SQL
# drivers); `use` names are flat either way.
vpath %.zm lib lib/core lib/db

CORELIB_HEADERS := $(CORELIB_MODULES:%=src/%_source.h)

$(OBJ): $(CORELIB_HEADERS)

# lib/core/<name>.zm -> src/<name>_source.h, a
# `#define <NAME>_SOURCE "..."` (module name uppercased). A #define, not
# a `static const char *`, so it's usable as a constant initializer in
# corelib.c's CORE_MODULES table -- a plain variable can't be (ISO C
# requires a static initializer to itself be a constant expression). One
# quoted string literal per source line, backslash-newline-continued
# across physical lines (same shape prelude.c's hand-written
# PRELUDE_SOURCE uses) rather than one unreadable giant line -- adjacent
# string literals concatenate into a single one regardless of how many
# physical lines or trailing "" the macro's definition spans.
src/%_source.h: %.zm
	awk -v src="$<" -v macro="$(shell echo $* | tr a-z A-Z)_SOURCE" \
	    'BEGIN { printf "/* Generated from %s -- do not edit directly. */\n#define %s \\\n", src, macro } \
	     { gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); printf "    \"%s\\n\" \\\n", $$0 } \
	     END { print "    \"\"" }' $< > $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEP)

test: $(BIN)
	./tests/run_tests.sh

# The Postgres driver tests: need docker/podman and a postgres:17 image
# (see tests/run_pg_tests.sh), so they are not part of `make test`.
test-pg: $(BIN)
	./tests/run_pg_tests.sh

# The TLS client tests: need the openssl binary (see tests/run_tls_tests.sh).
test-tls: $(BIN)
	./tests/run_tls_tests.sh

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

# docs/CORELIB.md is generated from the lib/core and lib/db modules' own `pub` declarations
# (see tools/gen_corelib_reference.sh) -- not run as part of `make`/`make
# all` since it's a doc, not a build input; run by hand after touching
# any core module's public API. The generated file is still checked into
# git (unlike src/*_source.h) since it's meant to be read on GitHub, not
# just consumed by the build.
corelib-reference:
	./tools/gen_corelib_reference.sh > docs/CORELIB.md

clean:
	rm -f $(BIN) src/*.o src/*.d src/*_source.h
