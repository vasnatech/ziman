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
# -Wvla and -Wshadow earn their place; the rest of a much higher warning level was triaged and
# rejected as noise (414 -Wswitch-enum by design, -Wlogical-op on the correct EAGAIN||EWOULDBLOCK
# idiom, and so on -- docs/reviews/tasks.md F14). -Wvla had exactly one hit in the tree and it was
# a real bug: the 96 KB variable-length array inside the regex matcher's recursion (A16).
PLAIN_CFLAGS := -std=c11 -O2 -fwrapv -Wall -Wextra -Wpedantic -Wvla -Wshadow -g -pthread
CFLAGS := $(PLAIN_CFLAGS) -DZIMAN_VERSION='"$(ZIMAN_VERSION)"' $(GC_FLAGS) -MMD -MP
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)
BIN := ziman

.PHONY: all check unit-tests clean fmt-check fmt-check-backend docs-check experimental-check test test-nogc nogc test-pg test-mysql test-tls test-lsp test-bignum test-decimal test-handles test-arena test-coverage test-gc-object test-gc-copy test-gc-lookup test-gc-scan test-gc-heap test-gc-mark test-gc-env-roots test-gc-interp-roots corelib-reference lib-manual-check not-yet-check syntax-check design-index design-index-check libc-free-check xalloc-check tls-if-available valgrind test-regex asan ubsan tsan sanitizers analyze

all: $(BIN)

# Every gate that needs nothing but this repo and a C toolchain: the fixture suite on both builds,
# the standalone unit tests, formatting, and the documentation guards. One command for "is this
# repo healthy", because the alternative is what actually happened -- `lib-manual-check` sat broken
# on main for a while, since `make test` does not run it and nothing else did either.
#
# Deliberately excluded, since each needs something the repo cannot provide: `test-pg` and
# `test-mysql` (a container runtime and a database image), `test-tls` (the `openssl` binary), and
# the benchmarks under tests/bench (`ab`, and a checkout of the sample backend next door).
check: test test-nogc unit-tests tls-if-available fmt-check docs-check libc-free-check xalloc-check

# The standalone C unit tests -- each links a few src/*.c files with its own main() and runs in
# well under a second, so there is no reason to ever run them selectively.
unit-tests: test-bignum test-decimal test-handles test-arena test-coverage test-gc-object test-gc-copy \
            test-regex \
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

# The TLS suite as part of `make check`, when it can run. It needs `openssl` on PATH -- the harness
# generates certificates with it and starts two `openssl s_server` processes -- which is why it was
# left out of `check` in the first place. Leaving it out had a cost: **no TLS code path in this
# project had ever run under a sanitizer**, and that is how a use-after-free on an `SSL` object
# survived A24, a review specifically about races in that file (docs/reviews/tasks.md A36, and A32
# for the bug). Skipped with a word rather than silently, so a machine without openssl still gets a
# green `check` and knows what it did not run.
tls-if-available: $(BIN)
	@if command -v openssl >/dev/null 2>&1; then \
	    ./tests/run_tls_tests.sh; \
	else \
	    echo "tls-if-available: skipped -- openssl is not on PATH, so the TLS suite cannot run"; \
	fi

# Unit tests for internal C modules that don't fit the language-level
# tests/fixtures harness (e.g. bignum.c's own arithmetic correctness,
# independent of any Ziman-level syntax). Each has its own main(), so it
# lives outside src/ (which this Makefile wildcards into the ziman
# binary) and is built/run standalone rather than joining $(OBJ).
test-bignum: src/xalloc.c src/bignum.c tests/unit/bignum_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_bignum_test src/xalloc.c src/bignum.c tests/unit/bignum_test.c
	/tmp/ziman_bignum_test

test-regex: src/regex.c tests/unit/regex_test.c
	$(CC) $(PLAIN_CFLAGS) -Wl,--wrap=malloc -o /tmp/ziman_regex_test src/regex.c tests/unit/regex_test.c
	/tmp/ziman_regex_test

test-handles: src/xalloc.c src/handles.c tests/unit/handles_test.c
	$(CC) $(PLAIN_CFLAGS) -Wl,--wrap=malloc -Wl,--wrap=calloc -o /tmp/ziman_handles_test src/xalloc.c src/handles.c tests/unit/handles_test.c
	/tmp/ziman_handles_test

test-decimal: src/xalloc.c src/bignum.c src/decimal.c tests/unit/decimal_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_decimal_test src/xalloc.c src/bignum.c src/decimal.c tests/unit/decimal_test.c
	/tmp/ziman_decimal_test

# src/arena.c doesn't need the GC/plain distinction (it's built on mmap/munmap directly, never
# malloc/free, precisely so it stays inert under either -- see its own header comment), but is
# tested the same standalone way as the other C modules above rather than through the interpreter.
# src/coverage.c alone: the lcov writer and, mainly, that writing into a file which already has
# records adds to them rather than replacing them. The AST is built by hand in the test, so nothing
# else links.
test-coverage: src/xalloc.c src/coverage.c tests/unit/coverage_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_coverage_test src/xalloc.c src/coverage.c tests/unit/coverage_test.c
	/tmp/ziman_coverage_test

test-arena: src/xalloc.c src/arena.c tests/unit/arena_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_arena_test src/xalloc.c src/arena.c tests/unit/arena_test.c
	/tmp/ziman_arena_test

# src/gc_object.c sits on top of src/arena.c (Phase 3's object-header layer, docs/MEMORY.md) --
# same standalone-test convention as arena.c itself, ahead of any mark/copy logic built on it.
test-gc-object: src/xalloc.c src/arena.c src/gc_object.c tests/unit/gc_object_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_object_test src/xalloc.c src/arena.c src/gc_object.c tests/unit/gc_object_test.c
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
test-gc-copy: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_copy.c tests/unit/gc_copy_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_copy_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_copy.c tests/unit/gc_copy_test.c
	/tmp/ziman_gc_copy_test

# src/gc_lookup.c resolves a bare address (a future conservative stack scan's own candidate words,
# docs/MEMORY.md Phase 3) to the gc_alloc'd object it falls within or inside, by walking arena.c's
# own block layout via gc_object.c's gc_next_object -- no interpreter Value machinery needed, so
# this stays as dependency-free as arena.c/gc_object.c's own tests, unlike gc_copy.c's.
test-gc-lookup: src/xalloc.c src/arena.c src/gc_object.c src/gc_lookup.c tests/unit/gc_lookup_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_lookup_test src/xalloc.c src/arena.c src/gc_object.c src/gc_lookup.c tests/unit/gc_lookup_test.c
	/tmp/ziman_gc_lookup_test

# src/gc_scan.c conservatively scans the calling thread's own C stack for roots into an arena, using
# src/stack.c's own stack-bounds computation (ziman_stack_bounds) and src/gc_lookup.c to resolve each
# candidate word -- see its own header comment on why this test cannot be fully valgrind-clean in the
# strict sense every earlier module here is: reading genuinely uninitialised stack memory and
# comparing it as a possible pointer is what conservative scanning *is*, not a bug (the same
# interaction Boehm's own collector has with valgrind).
#
# `valgrind --undef-value-errors=no` used to be the advice here, and it is no longer the best one:
# it switches off memcheck's uninitialised-value checking *process-wide*, which is the one check
# nothing else in this toolchain provides. `make valgrind` instead runs this test, its neighbours
# and three fixtures with `tools/valgrind.supp`, which suppresses exactly the conservative scan's
# own reads and leaves that checking on everywhere else -- 0 errors from 0 contexts, 215
# suppressed (docs/reviews/tasks.md F23).
test-gc-scan: src/xalloc.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/stack.c src/error.c tests/unit/gc_scan_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_scan_test src/xalloc.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/stack.c src/error.c tests/unit/gc_scan_test.c
	/tmp/ziman_gc_scan_test

# src/gc_heap.c is Phase 3's actual collected heap -- a free-list-capable allocator on top of
# arena.c/gc_object.c, needed once Phase 3 switched from a copying design to mark-sweep (a
# conservative stack scan's own roots can never safely be relocated; see gc_heap.h's own header
# comment for why, and docs/MEMORY.md for the fuller story). gc_heap_collect wires gc_scan.c and
# gc_mark.c together with gc_heap.c's own alloc/sweep, so this now needs their full dependency
# chain too (real ArrayObj/StructObj/etc. construction for gc_mark.c, stack.c/error.c for
# gc_scan.c), not just arena.c/gc_object.c on their own.
test-gc-heap: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_env_roots.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/unit/gc_heap_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_heap_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_env_roots.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/unit/gc_heap_test.c
	/tmp/ziman_gc_heap_test

# src/gc_mark.c is the mark phase mark-sweep needs: walks a real Value graph by type.kind (mirroring
# gc_copy.c's own per-kind structure) and calls gc_set_marked instead of copying. Needs the same
# dependency chain as gc_copy_test.c's own (real ArrayObj/StructObj/etc. construction), plus
# gc_lookup.c (interior-pointer resolution for string views and array slices during marking).
# value.c's own value_string_copy now allocates through gc_thread_heap.c/gc_heap.c, which pulls in
# gc_scan.c transitively too.
test-gc-mark: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/unit/gc_mark_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_mark_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/unit/gc_mark_test.c
	/tmp/ziman_gc_mark_test

# src/gc_env_roots.c is the second root source a real collection cycle needs besides the
# conservative stack scan: an Env (env.h) lives in the ordinary allocator's own heap, never a
# thread's GcHeap arena, so once a Value is stored into a binding its own pointer fields move off
# the C stack (where gc_scan_stack_roots could see them) into Env.items' own heap storage --
# invisible to a scan that only reads raw stack bytes and never dereferences what it finds. Every
# ordinary variable is affected, not just interpreter.c's t_defer_envs/t_env_pool, which is where
# this gap was originally (too narrowly) expected to live -- see gc_env_roots.h's own header
# comment and docs/MEMORY.md for the fuller story.
test-gc-env-roots: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c tests/unit/gc_env_roots_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_env_roots_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c tests/unit/gc_env_roots_test.c
	/tmp/ziman_gc_env_roots_test

# src/gc_interp_roots.c aggregates every root source a real collection cycle needs beyond the
# conservative stack scan: the global scope, every active call's own frame (via error.c's frame
# tracking), and interpreter.c's own t_defer_envs/t_env_pool -- taken as explicit parameters rather
# than read from interpreter.c's private state directly, since interpreter.c is too large to link
# into a standalone test the way every other module here has been. This test builds synthetic
# stand-ins for that state, exercising the identical code path a real interpreter would.
test-gc-interp-roots: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c src/gc_interp_roots.c tests/unit/gc_interp_roots_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_interp_roots_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c src/gc_interp_roots.c tests/unit/gc_interp_roots_test.c
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

# Regenerates docs/DESIGN.md's section index in place -- see tools/gen_design_index.py.
design-index:
	python3 tools/gen_design_index.py

# Fails when the committed index is not what the sections now are: the same `git diff` shape
# lib-manual-check uses, because a hand-kept index of a file that size is wrong within a month.
design-index-check:
	python3 tools/gen_design_index.py
	git diff --exit-code --stat -- docs/DESIGN.md

# Fails when a Ziman example in docs/SYNTAX.md no longer compiles (or, if it is tagged `fails`,
# when it starts compiling) -- see tools/check_syntax_examples.py's own header comment. Needs the
# binary, since it compiles every example with it.
syntax-check: $(BIN)
	python3 tools/check_syntax_examples.py

# Every guard that keeps the documentation from drifting from the sources it describes, in one
# target, because each of these is only useful if something actually runs it: `lib-manual-check`
# was failing on main for a while precisely because nothing did.
# ---------------------------------------------------------------- sanitizers
#
# One target per sanitizer: build the plain (no-collector) configuration with it and run the whole
# fixture suite. That configuration is the one that exercises this project's own collector, which is
# where most of the C is; for the Boehm configuration add $(GC_FLAGS) and $(GC_LIB) to the line, as
# the `gc` target does.
#
# There was no such target until 2026-10-08, so every review built its own -- and a standing one
# would have caught A19's data race and A22's undefined memcpy without a review at all
# (docs/reviews/tasks.md F14). Three things these targets know that a fresh attempt would have to
# rediscover:
#
#   * halt_on_error=1 everywhere. A sanitizer report goes to stderr, and a fixture checked against a
#     .out file passes regardless of what lands on stderr -- so without halting, the suite reports
#     "888 passed" while printing undefined behaviour.
#   * detect_stack_use_after_return=0 for ASan -- and the reason is the collector, not the depth
#     guard, which is what this comment used to say (docs/reviews/tasks.md F22). The guard does
#     break: its probe is a local's address, and the fake stack moves locals, so with the option on
#     nothing runs. But that part is fixable -- taking the probe from __builtin_frame_address(0)
#     under __SANITIZE_ADDRESS__ works, because the fake stack moves locals and not the frame
#     pointer. What is not fixable is that the interpreter's locals then live in ASan's
#     heap-allocated fake frames, outside the range gc_scan.c walks: measured here over 60 fixtures,
#     the conservative scan finds **45 roots with the option on against 92 with it off**, a 51%
#     loss, and the suite passes either way -- so no test can tell you the collector has gone
#     unsound. bdwgc hard-codes the same option off (vendor/bdwgc/os_dep.c:560-590) with the comment
#     "TODO: support ASan fake stacks properly". Closing it for real means teaching gc_scan.c about
#     __asan_get_current_fake_stack.
#   * SKIP, for one fixture each, and for different reasons. Under ASan,
#     stack_depth_deep_recursion_works no longer fits: instrumented frames are several times larger,
#     so 20,000 calls deep passes the 256 MB its own .args asks for. Under TSan,
#     stack_depth_trace_bounded dies in TSan's own reporting -- a SEGV inside std::ostream::flush,
#     because the fixture deliberately exhausts the stack and the reporter needs stack of its own.
#     Every other stack-depth fixture reports its error normally under both. Neither is a bug in
#     anything, and both were found by running these targets rather than reasoned about.
#
# On its first run `make tsan` found two real races -- one on a socket handle's descriptor and one
# TSan was right to flag and wrong about (a signal handler writing a `volatile sig_atomic_t`, which
# the standard permits and a sanitizer cannot see). Both are fixed, A24 in docs/reviews/tasks.md, and
# all three targets are green on this tree. That is the argument for having them: nothing else here
# could have found either.
#
# The runtime libraries are packaged separately on some distributions (libasan, libubsan, libtsan on
# Fedora) and were not installed on the machine these targets were written on, so each recipe passes
# $(LDFLAGS) through: a runtime unpacked elsewhere can be used without editing this file, and a link
# that fails for that reason says so instead of printing a wall of ld output.
SAN_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fno-omit-frame-pointer -pthread
SAN_LIBS := -lm -lsqlite3 -lssl -lcrypto -lcrypt -lz -pthread
# Deliberately without -fwrapv, which the ordinary build carries: with it, signed overflow is
# defined and UBSan's integer checks have nothing to say. This is the build that found A21.
UBSAN_CHECKS := -fsanitize=undefined,signed-integer-overflow,float-divide-by-zero,float-cast-overflow,bounds-strict,pointer-overflow,unreachable

asan: $(SRC) $(CORELIB_HEADERS)
	@$(CC) $(SAN_CFLAGS) -fwrapv -fsanitize=address -DZIMAN_VERSION='"$(ZIMAN_VERSION)-asan"' \
	    -o ziman-asan $(SRC) $(LDFLAGS) $(SAN_LIBS) 2> .asan-link.log \
	    || { cat .asan-link.log; rm -f .asan-link.log; \
	         echo "make asan: that link needs the AddressSanitizer runtime (libasan), which this machine may not have"; \
	         echo "  install it (Fedora: dnf install libasan), or point at an unpacked copy:"; \
	         echo "  make asan LDFLAGS=\"-L/path/to/lib -Wl,-rpath,/path/to/lib\""; exit 1; }
	@rm -f .asan-link.log
	ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0:halt_on_error=1 \
	    BIN=./ziman-asan SKIP=stack_depth_deep_recursion_works ./tests/run_tests.sh
	@if command -v openssl >/dev/null 2>&1; then \
	    ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0:halt_on_error=1 BIN=./ziman-asan ./tests/run_tls_tests.sh; \
	else echo "make asan: TLS suite skipped -- openssl is not on PATH"; fi

ubsan: $(SRC) $(CORELIB_HEADERS)
	@$(CC) $(SAN_CFLAGS) $(UBSAN_CHECKS) -DZIMAN_VERSION='"$(ZIMAN_VERSION)-ubsan"' \
	    -o ziman-ubsan $(SRC) $(LDFLAGS) $(SAN_LIBS) 2> .ubsan-link.log \
	    || { cat .ubsan-link.log; rm -f .ubsan-link.log; \
	         echo "make ubsan: that link needs the UndefinedBehaviorSanitizer runtime (libubsan), which this machine may not have"; \
	         echo "  install it (Fedora: dnf install libubsan), or point at an unpacked copy:"; \
	         echo "  make ubsan LDFLAGS=\"-L/path/to/lib -Wl,-rpath,/path/to/lib\""; exit 1; }
	@rm -f .ubsan-link.log
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	    BIN=./ziman-ubsan ./tests/run_tests.sh
	@if command -v openssl >/dev/null 2>&1; then \
	    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 BIN=./ziman-ubsan ./tests/run_tls_tests.sh; \
	else echo "make ubsan: TLS suite skipped -- openssl is not on PATH"; fi

tsan: $(SRC) $(CORELIB_HEADERS)
	@$(CC) $(SAN_CFLAGS) -fwrapv -fsanitize=thread -DZIMAN_VERSION='"$(ZIMAN_VERSION)-tsan"' \
	    -o ziman-tsan $(SRC) $(LDFLAGS) $(SAN_LIBS) 2> .tsan-link.log \
	    || { cat .tsan-link.log; rm -f .tsan-link.log; \
	         echo "make tsan: that link needs the ThreadSanitizer runtime (libtsan), which this machine may not have"; \
	         echo "  install it (Fedora: dnf install libtsan), or point at an unpacked copy:"; \
	         echo "  make tsan LDFLAGS=\"-L/path/to/lib -Wl,-rpath,/path/to/lib\""; exit 1; }
	@rm -f .tsan-link.log
	TSAN_OPTIONS=halt_on_error=1:history_size=4 \
	    BIN=./ziman-tsan SKIP=stack_depth_trace_bounded ./tests/run_tests.sh
	@if command -v openssl >/dev/null 2>&1; then \
	    TSAN_OPTIONS=halt_on_error=1:history_size=4 BIN=./ziman-tsan ./tests/run_tls_tests.sh; \
	else echo "make tsan: TLS suite skipped -- openssl is not on PATH"; fi

sanitizers: asan ubsan tsan

# memcheck, narrowly -- see tests/run_valgrind.sh for what it covers and why it is not a whole-suite
# sweep, and tools/valgrind.supp for the conservative-scanner suppressions it needs. Built with
# -fno-omit-frame-pointer, without which those suppressions do not match (several contexts unwind
# as `???` and the frame they key on never appears). Not in `make check`: valgrind is about 100x
# slower than ASan. What it buys that the sanitizer targets cannot is uninitialised-value checking,
# which ASan does not do and MSan is not here to do, plus the leak checker that found A26.
ziman-vg: $(SRC) $(CORELIB_HEADERS)
	$(CC) $(PLAIN_CFLAGS) -fno-omit-frame-pointer -DZIMAN_VERSION='"$(ZIMAN_VERSION)-vg"' \
	    -o ziman-vg $(SRC) -lm -lsqlite3 -lssl -lcrypto -lcrypt -lz -pthread

valgrind: ziman-vg test-gc-scan test-gc-lookup test-arena test-gc-object test-gc-interp-roots test-handles test-coverage
	./tests/run_valgrind.sh ./ziman-vg

# GCC's static analyzer over both configurations. Clean as of 2026-10-08 and deliberately not part of
# `make check`: it takes minutes per configuration, where every other check here takes seconds.
analyze:
	$(CC) -std=c11 -O1 -fwrapv -pthread -fanalyzer -fsyntax-only $(GC_FLAGS) $(SRC)
	$(CC) -std=c11 -O1 -fwrapv -pthread -fanalyzer -fsyntax-only $(SRC)
	@echo "analyze: -fanalyzer clean on both configurations"

libc-free-check:
	python3 tools/check_libc_free.py

xalloc-check:
	python3 tools/check_xalloc.py

docs-check: lib-manual-check not-yet-check experimental-check syntax-check design-index-check

clean:
	rm -f $(BIN) ziman-nogc ziman-allocprofile ziman-asan ziman-ubsan ziman-tsan ziman-vg src/*.o src/*.d src/*_source.h $(GC_DIR)/gc.o $(GC_LIB)
