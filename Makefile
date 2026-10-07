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

.PHONY: all check unit-tests clean fmt-check fmt-check-backend docs-check experimental-check test test-nogc nogc test-pg test-mysql test-tls test-lsp test-bignum test-decimal test-handles test-arena test-gc-object test-gc-copy test-gc-lookup test-gc-scan test-gc-heap test-gc-mark test-gc-env-roots test-gc-interp-roots corelib-reference lib-manual-check not-yet-check syntax-check

all: $(BIN)

# Every gate that needs nothing but this repo and a C toolchain: the fixture suite on both builds,
# the standalone unit tests, formatting, and the documentation guards. One command for "is this
# repo healthy", because the alternative is what actually happened -- `lib-manual-check` sat broken
# on main for a while, since `make test` does not run it and nothing else did either.
#
# Deliberately excluded, since each needs something the repo cannot provide: `test-pg` and
# `test-mysql` (a container runtime and a database image), `test-tls` (the `openssl` binary), and
# the benchmarks under tests/bench (`ab`, and a checkout of the sample backend next door).
check: test test-nogc unit-tests fmt-check docs-check

# The standalone C unit tests -- each links a few src/*.c files with its own main() and runs in
# well under a second, so there is no reason to ever run them selectively.
unit-tests: test-bignum test-decimal test-handles test-arena test-gc-object test-gc-copy \
            test-gc-lookup test-gc-scan test-gc-heap test-gc-mark test-gc-env-roots \
            test-gc-interp-roots test-lsp

$(GC_LIB): $(GC_DIR)/extra/gc.c $(wildcard $(GC_DIR)/*.c) $(wildcard $(GC_DIR)/include/*.h $(GC_DIR)/include/*/*.h)
	$(CC) -O2 -DNDEBUG -I$(GC_DIR)/include -DGC_THREADS -DGC_BUILTIN_ATOMIC -DTHREAD_LOCAL_ALLOC -DPARALLEL_MARK \
	    -DUSE_MMAP -DALL_INTERIOR_POINTERS -D_GNU_SOURCE -pthread -c $(GC_DIR)/extra/gc.c -o $(GC_DIR)/gc.o
	rm -f $@
	ar rcs $@ $(GC_DIR)/gc.o

$(BIN): $(OBJ) $(GC_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(GC_LIB) -lm -lsqlite3 -lssl -lcrypto -lcrypt -lz -pthread

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
CORELIB_MODULES := collections url fs log metrics regex client random uuid clock time json number math bits thread datasource sqlite mysql db net crypto pg http server os properties ndjson csv toml yaml compress zone mime jwt migrate otp test prelude
# Bundled modules live in lib/core (general-purpose), lib/db (SQL
# drivers) lib/http (http, server, client) and lib/time (time, zone); `use` names are flat either way.
vpath %.zm lib lib/core lib/db lib/http lib/time

CORELIB_HEADERS := $(CORELIB_MODULES:%=src/%_source.h)

$(OBJ): $(CORELIB_HEADERS)

ziman-nogc: $(SRC) $(CORELIB_HEADERS)
	$(CC) $(PLAIN_CFLAGS) -DZIMAN_VERSION='"$(ZIMAN_VERSION)-nogc"' -o $@ $(SRC) -lm -lsqlite3 -lssl -lcrypto -lcrypt -lz -pthread

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
# The bundled library and the extra test programs are kept formatted by `ziman --fmt`
# (tests/fixtures is not: fixtures that test error positions depend on their exact layout).
#
# This repo only. The sample backend lives in a sibling checkout and has its own target below --
# it used to be in this one, which made a gate on *this* repo fail for a file in another, and meant
# anyone without that checkout could not run `make fmt-check` at all. Found when `make check` first
# ran the whole set together: it had been failing on three of the backend's files.
fmt-check: $(BIN)
	./$(BIN) --fmt --check $$(find lib docs/lib/examples tests/pg tests/mysql tests/tls tests/shutdown tests/bench -name '*.zm')

# The sample backend (health-record-backend-ziman), which is kept formatted the same way but lives
# next door, so this is separate from `fmt-check` and skips cleanly when it is not checked out.
BACKEND_DIR := ../health-record/health-record-backend-ziman
fmt-check-backend: $(BIN)
	@if [ -d "$(BACKEND_DIR)/src" ]; then \
	    ./$(BIN) --fmt --check $$(find $(BACKEND_DIR)/src -name '*.zm'); \
	else \
	    echo "SKIP fmt-check-backend: $(BACKEND_DIR)/src is not checked out"; \
	fi

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

test-handles: src/handles.c tests/unit/handles_test.c
	$(CC) $(PLAIN_CFLAGS) -Wl,--wrap=malloc -Wl,--wrap=calloc -o /tmp/ziman_handles_test src/handles.c tests/unit/handles_test.c
	/tmp/ziman_handles_test

test-decimal: src/bignum.c src/decimal.c tests/unit/decimal_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_decimal_test src/bignum.c src/decimal.c tests/unit/decimal_test.c
	/tmp/ziman_decimal_test

# src/arena.c doesn't need the GC/plain distinction (it's built on mmap/munmap directly, never
# malloc/free, precisely so it stays inert under either -- see its own header comment), but is
# tested the same standalone way as the other C modules above rather than through the interpreter.
test-arena: src/arena.c tests/unit/arena_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_arena_test src/arena.c tests/unit/arena_test.c
	/tmp/ziman_arena_test

# src/gc_object.c sits on top of src/arena.c (Phase 3's object-header layer, docs/MEMORY.md) --
# same standalone-test convention as arena.c itself, ahead of any mark/copy logic built on it.
test-gc-object: src/arena.c src/gc_object.c tests/unit/gc_object_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_object_test src/arena.c src/gc_object.c tests/unit/gc_object_test.c
	/tmp/ziman_gc_object_test

# src/gc_copy.c is src/value_copy.c's logic generalised onto gc_object.c's headers, with a dedup
# map added for sharing preservation (docs/MEMORY.md, "Phase 3 design"). Needs value.c's own
# ArrayObj/StructObj/etc. constructors, and everything value.c itself needs (ast.c for Type, error.c
# and stack.c for the runtime-error/recursion-depth paths value.c can hit, bignum.c/decimal.c,
# mapobj.c) -- unlike arena.c/gc_object.c this can't stay dependency-free, since it exercises the
# real Value representation, not a synthetic stand-in. value_hash (normally in src/interpreter.c,
# not linked here) is stubbed inside the test itself; see its own header comment for why that's fine.
# value.c's own value_string_copy now allocates through src/gc_thread_heap.c/gc_heap.c (docs/DESIGN.md's
# "narrowest possible slice"), which pulls in gc_lookup.c/gc_scan.c/gc_mark.c transitively too.
test-gc-copy: src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_copy.c tests/unit/gc_copy_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_copy_test src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_copy.c tests/unit/gc_copy_test.c
	/tmp/ziman_gc_copy_test

# src/gc_lookup.c resolves a bare address (a future conservative stack scan's own candidate words,
# docs/MEMORY.md Phase 3) to the gc_alloc'd object it falls within or inside, by walking arena.c's
# own block layout via gc_object.c's gc_next_object -- no interpreter Value machinery needed, so
# this stays as dependency-free as arena.c/gc_object.c's own tests, unlike gc_copy.c's.
test-gc-lookup: src/arena.c src/gc_object.c src/gc_lookup.c tests/unit/gc_lookup_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_lookup_test src/arena.c src/gc_object.c src/gc_lookup.c tests/unit/gc_lookup_test.c
	/tmp/ziman_gc_lookup_test

# src/gc_scan.c conservatively scans the calling thread's own C stack for roots into an arena, using
# src/stack.c's own stack-bounds computation (ziman_stack_bounds) and src/gc_lookup.c to resolve each
# candidate word -- see its own header comment on why this test cannot be fully valgrind-clean in the
# strict sense every earlier module here is: reading genuinely uninitialised stack memory and
# comparing it as a possible pointer is what conservative scanning *is*, not a bug (the same
# interaction Boehm's own collector has with valgrind). `valgrind --undef-value-errors=no` (memcheck's
# other checks -- invalid read/write, leak detection -- still fully active) is the honest way to
# confirm there is no *actual* memory-safety issue underneath that expected noise.
test-gc-scan: src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/stack.c src/error.c tests/unit/gc_scan_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_scan_test src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/stack.c src/error.c tests/unit/gc_scan_test.c
	/tmp/ziman_gc_scan_test

# src/gc_heap.c is Phase 3's actual collected heap -- a free-list-capable allocator on top of
# arena.c/gc_object.c, needed once Phase 3 switched from a copying design to mark-sweep (a
# conservative stack scan's own roots can never safely be relocated; see gc_heap.h's own header
# comment for why, and docs/MEMORY.md for the fuller story). gc_heap_collect wires gc_scan.c and
# gc_mark.c together with gc_heap.c's own alloc/sweep, so this now needs their full dependency
# chain too (real ArrayObj/StructObj/etc. construction for gc_mark.c, stack.c/error.c for
# gc_scan.c), not just arena.c/gc_object.c on their own.
test-gc-heap: src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_env_roots.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/unit/gc_heap_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_heap_test src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_env_roots.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/unit/gc_heap_test.c
	/tmp/ziman_gc_heap_test

# src/gc_mark.c is the mark phase mark-sweep needs: walks a real Value graph by type.kind (mirroring
# gc_copy.c's own per-kind structure) and calls gc_set_marked instead of copying. Needs the same
# dependency chain as gc_copy_test.c's own (real ArrayObj/StructObj/etc. construction), plus
# gc_lookup.c (interior-pointer resolution for string views and array slices during marking).
# value.c's own value_string_copy now allocates through gc_thread_heap.c/gc_heap.c, which pulls in
# gc_scan.c transitively too.
test-gc-mark: src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/unit/gc_mark_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_mark_test src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/unit/gc_mark_test.c
	/tmp/ziman_gc_mark_test

# src/gc_env_roots.c is the second root source a real collection cycle needs besides the
# conservative stack scan: an Env (env.h) lives in the ordinary allocator's own heap, never a
# thread's GcHeap arena, so once a Value is stored into a binding its own pointer fields move off
# the C stack (where gc_scan_stack_roots could see them) into Env.items' own heap storage --
# invisible to a scan that only reads raw stack bytes and never dereferences what it finds. Every
# ordinary variable is affected, not just interpreter.c's t_defer_envs/t_env_pool, which is where
# this gap was originally (too narrowly) expected to live -- see gc_env_roots.h's own header
# comment and docs/MEMORY.md for the fuller story.
test-gc-env-roots: src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c tests/unit/gc_env_roots_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_env_roots_test src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c tests/unit/gc_env_roots_test.c
	/tmp/ziman_gc_env_roots_test

# src/gc_interp_roots.c aggregates every root source a real collection cycle needs beyond the
# conservative stack scan: the global scope, every active call's own frame (via error.c's frame
# tracking), and interpreter.c's own t_defer_envs/t_env_pool -- taken as explicit parameters rather
# than read from interpreter.c's private state directly, since interpreter.c is too large to link
# into a standalone test the way every other module here has been. This test builds synthetic
# stand-ins for that state, exercising the identical code path a real interpreter would.
test-gc-interp-roots: src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c src/gc_interp_roots.c tests/unit/gc_interp_roots_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_interp_roots_test src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c src/gc_interp_roots.c tests/unit/gc_interp_roots_test.c
	/tmp/ziman_gc_interp_roots_test

# docs/CORELIB.md and the manual in docs/lib/ are generated from the lib/core and lib/db modules' own `pub` declarations
# (see tools/gen_corelib_reference.sh) -- not run as part of `make`/`make
# all` since it's a doc, not a build input; run by hand after touching
# any core module's public API. The generated file is still checked into
# git (unlike src/*_source.h) since it's meant to be read on GitHub, not
# just consumed by the build.
corelib-reference:
	./tools/gen_corelib_reference.sh > docs/CORELIB.md
	python3 tools/gen_lib_manual.py
	python3 tools/gen_lib_deps.py

# Fails when docs/lib is not what the current lib/ sources generate (run `make corelib-reference`).
lib-manual-check: $(BIN)
	python3 tools/gen_lib_manual.py
	python3 tools/gen_lib_deps.py
	git diff --exit-code --stat -- docs/lib

# Fails when docs/SYNTAX.md or a lib/**/*.zm doc comment claims some `pub` name is "not yet"
# supported when it's already declared -- see tools/check_not_yet.py's own header comment.
not-yet-check:
	python3 tools/check_not_yet.py

# Fails when README.md's module table disagrees with the modules' own `#![experimental]`
# attribute -- see tools/check_experimental.py's own header comment.
experimental-check:
	python3 tools/check_experimental.py

# ./ziman-allocprofile: the collector build plus a count of every allocation against its source
# site, dumped to stderr at exit, hottest first. Not a normal build and not fast -- it takes a mutex
# per allocation -- but it answers "what actually allocates" exactly rather than by sampling, which is
# how F7's three hottest sites were found (the review had named two different ones). See
# src/allocsites.c. Run as: ./ziman-allocprofile <program.zm> 2>&1 | sed -n '/allocations by site/,$p'
ziman-allocprofile: $(SRC) $(CORELIB_HEADERS) $(GC_LIB)
	$(CC) $(PLAIN_CFLAGS) -DZIMAN_VERSION='"$(ZIMAN_VERSION)-allocprofile"' $(GC_FLAGS) 	    -DZIMAN_ALLOC_PROFILE -o $@ $(SRC) $(GC_LIB) -lm -lsqlite3 -lssl -lcrypto -lcrypt -lz -pthread

# Fails when a Ziman example in docs/SYNTAX.md no longer compiles (or, if it is tagged `fails`,
# when it starts compiling) -- see tools/check_syntax_examples.py's own header comment. Needs the
# binary, since it compiles every example with it.
syntax-check: $(BIN)
	python3 tools/check_syntax_examples.py

# Every guard that keeps the documentation from drifting from the sources it describes, in one
# target, because each of these is only useful if something actually runs it: `lib-manual-check`
# was failing on main for a while precisely because nothing did.
docs-check: lib-manual-check not-yet-check experimental-check syntax-check

clean:
	rm -f $(BIN) ziman-nogc ziman-allocprofile src/*.o src/*.d src/*_source.h $(GC_DIR)/gc.o $(GC_LIB)
