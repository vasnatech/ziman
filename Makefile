CC := cc
# -fwrapv: signed overflow in the C code itself is well defined (wraps) instead of
# undefined behaviour the optimiser may exploit. Ziman's own `int` arithmetic
# does not rely on it -- it traps on overflow explicitly (see interpreter.c).
ZIMAN_VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

# Every allocation of the interpreter goes through the Boehm-Demers-Weiser
# collector (src/zgc.h is force-included into every source file and redirects
# malloc/calloc/realloc/strdup to it; `free` becomes a no-op), so a long-running
# program's memory is reclaimed instead of growing forever. The collector is
# vendored (vendor/bdwgc, MIT-style license) and built here with the options
# Ziman needs, so building needs no network and no other tools. `make nogc`
# builds ./ziman-nogc without it (nothing is ever freed) -- for measuring, or a
# platform the collector does not support. See docs/DESIGN.md, "Memory reclamation".
GC_DIR := vendor/bdwgc
GC_LIB := $(GC_DIR)/libgc.a
GC_FLAGS := -DZIMAN_GC -DGC_THREADS -include src/zgc.h -I$(GC_DIR)/include -D_GNU_SOURCE

# -fwrapv: signed overflow in the C code itself is well defined (wraps) instead of
# undefined behaviour the optimiser may exploit. Ziman's own `int` arithmetic
# does not rely on it -- it traps on overflow explicitly (see interpreter.c).
PLAIN_CFLAGS := -std=c11 -O2 -fwrapv -Wall -Wextra -Wpedantic -g -pthread
CFLAGS := $(PLAIN_CFLAGS) -DZIMAN_VERSION='"$(ZIMAN_VERSION)"' $(GC_FLAGS) -MMD -MP
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)
BIN := ziman

.PHONY: all clean fmt-check test test-nogc nogc test-pg test-mysql test-tls test-lsp test-bignum test-decimal corelib-reference lib-manual-check

all: $(BIN)

$(GC_LIB): $(GC_DIR)/extra/gc.c $(wildcard $(GC_DIR)/*.c) $(wildcard $(GC_DIR)/include/*.h $(GC_DIR)/include/*/*.h)
	$(CC) -O2 -DNDEBUG -I$(GC_DIR)/include -DGC_THREADS -DGC_BUILTIN_ATOMIC -DTHREAD_LOCAL_ALLOC -DPARALLEL_MARK \
	    -DUSE_MMAP -DALL_INTERIOR_POINTERS -D_GNU_SOURCE -pthread -c $(GC_DIR)/extra/gc.c -o $(GC_DIR)/gc.o
	rm -f $@
	ar rcs $@ $(GC_DIR)/gc.o

$(BIN): $(OBJ) $(GC_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(GC_LIB) -lm -lsqlite3 -lssl -lcrypto -lz -pthread

nogc: ziman-nogc

test-nogc: ziman-nogc
	BIN=./ziman-nogc ./tests/run_tests.sh

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
CORELIB_MODULES := collections fs log regex client random uuid clock time json number sql dburl sqlite mysql db net crypto pg http server os properties ndjson csv toml yaml compress prelude
# Bundled modules live in lib/core (general-purpose), lib/db (SQL
# drivers) and lib/http (http, server, client); `use` names are flat either way.
vpath %.zm lib lib/core lib/db lib/http

CORELIB_HEADERS := $(CORELIB_MODULES:%=src/%_source.h)

$(OBJ): $(CORELIB_HEADERS)

ziman-nogc: $(SRC) $(CORELIB_HEADERS)
	$(CC) $(PLAIN_CFLAGS) -DZIMAN_VERSION='"$(ZIMAN_VERSION)-nogc"' -o $@ $(SRC) -lm -lsqlite3 -lssl -lcrypto -lz -pthread

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

# The MySQL driver tests: need docker/podman and a mysql:8.0 image
# (see tests/run_mysql_tests.sh).
test-mysql: $(BIN)
	./tests/run_mysql_tests.sh

# The TLS client tests: need the openssl binary (see tests/run_tls_tests.sh).
# The bundled library, the extra test programs and the sample backend are kept
# formatted by `ziman --fmt` (tests/fixtures is not: fixtures that test error
# positions depend on their exact layout).
fmt-check: $(BIN)
	./$(BIN) --fmt --check $$(find lib docs/lib/examples tests/pg tests/mysql tests/tls tests/shutdown tests/bench -name '*.zm') ../health-record/health-record-backend-ziman/*.zm ../health-record/health-record-backend-ziman/*/*.zm

test-lsp: $(BIN)
	python3 tests/lsp/lsp_test.py ./$(BIN)

test-tls: $(BIN)
	./tests/run_tls_tests.sh

# Unit tests for internal C modules that don't fit the language-level
# tests/fixtures harness (e.g. bignum.c's own arithmetic correctness,
# independent of any Ziman-level syntax). Each has its own main(), so it
# lives outside src/ (which this Makefile wildcards into the ziman
# binary) and is built/run standalone rather than joining $(OBJ).
test-bignum: src/bignum.c tests/unit/bignum_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_bignum_test src/bignum.c tests/unit/bignum_test.c
	/tmp/ziman_bignum_test

test-decimal: src/bignum.c src/decimal.c tests/unit/decimal_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_decimal_test src/bignum.c src/decimal.c tests/unit/decimal_test.c
	/tmp/ziman_decimal_test

# docs/CORELIB.md and the manual in docs/lib/ are generated from the lib/core and lib/db modules' own `pub` declarations
# (see tools/gen_corelib_reference.sh) -- not run as part of `make`/`make
# all` since it's a doc, not a build input; run by hand after touching
# any core module's public API. The generated file is still checked into
# git (unlike src/*_source.h) since it's meant to be read on GitHub, not
# just consumed by the build.
corelib-reference:
	./tools/gen_corelib_reference.sh > docs/CORELIB.md
	python3 tools/gen_lib_manual.py

# Fails when docs/lib is not what the current lib/ sources generate (run `make corelib-reference`).
lib-manual-check: $(BIN)
	python3 tools/gen_lib_manual.py
	git diff --exit-code --stat -- docs/lib

clean:
	rm -f $(BIN) ziman-nogc src/*.o src/*.d src/*_source.h $(GC_DIR)/gc.o $(GC_LIB)
