# make/c.mk -- building the interpreter, and everything that checks the C.
#
# Every target here is about src/*.c: the binary and its variants, the standalone C unit tests
# under tests/c/, the three sanitizers, Memcheck, the static analyzer, and the source-level checks
# that read the C looking for an unchecked allocation or a missing initialiser.
#
# Variables live in the top-level Makefile, never here, so nothing can be evaluated before the
# thing it reads is defined.

all: $(BIN)

# The standalone C unit tests -- each links a few src/*.c files with its own main() and runs in
# well under a second, so there is no reason to ever run them selectively. `lib-tests` is the same
# idea one level up, in Ziman rather than C.
unit-tests: test-bignum test-decimal test-handles test-arena test-coverage test-gc-object test-gc-copy \
            test-regex lib-tests \
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

$(OBJ): $(CORELIB_HEADERS)

ziman-nogc: $(SRC) $(HDR) $(CORELIB_HEADERS)
	$(CC) $(PLAIN_CFLAGS) -DZIMAN_VERSION='"$(ZIMAN_VERSION)-nogc"' -o $@ $(SRC) -lm -lsqlite3 -lssl -lcrypto -lcrypt -lz -pthread

# lib/core/<name>.zm -> build/<name>_source.h, a
# `#define <NAME>_SOURCE "..."` (module name uppercased). A #define, not
# a `static const char *`, so it's usable as a constant initializer in
# corelib.c's CORE_MODULES table -- a plain variable can't be (ISO C
# requires a static initializer to itself be a constant expression). One
# Every `?` is escaped as `\?` because trigraphs are active under `-std=c11`
# (they are not under `-std=gnu11`): without it the nine `??x` sequences are
# rewritten inside the embedded library before the compiler ever sees them, so
# a Ziman source containing `??!` would arrive as `|`. Verified: `"a??!b"`
# prints `a|b` under -std=c11 and `a??!b` under -std=gnu11. `\?` is a valid C
# escape for `?` and breaks the sequence, so nothing else changes (F131).
# quoted string literal per source line, backslash-newline-continued
# across physical lines (same shape prelude.c's hand-written
# PRELUDE_SOURCE uses) rather than one unreadable giant line -- adjacent
# string literals concatenate into a single one regardless of how many
# physical lines or trailing "" the macro's definition spans.
$(BUILD)/%_source.h: %.zm | $(BUILD)
	awk -v src="$<" -v macro="$(shell echo $* | tr a-z A-Z)_SOURCE" \
	    'BEGIN { printf "/* Generated from %s -- do not edit directly. */\n#define %s \\\n", src, macro } \
	     { gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); gsub(/\?/, "\\?"); printf "    \"%s\\n\" \\\n", $$0 } \
	     END { print "    \"\"" }' $< > $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	@mkdir -p $(BUILD)

# Unit tests for internal C modules that don't fit the language-level
# tests/lang harness (e.g. bignum.c's own arithmetic correctness,
# independent of any Ziman-level syntax). Each has its own main(), so it
# lives outside src/ (which this Makefile wildcards into the ziman
# binary) and is built/run standalone rather than joining $(OBJ).
test-bignum: src/xalloc.c src/bignum.c tests/c/bignum_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_bignum_test src/xalloc.c src/bignum.c tests/c/bignum_test.c
	/tmp/ziman_bignum_test

test-regex: src/regex.c tests/c/regex_test.c
	$(CC) $(PLAIN_CFLAGS) -Wl,--wrap=malloc -o /tmp/ziman_regex_test src/regex.c tests/c/regex_test.c
	/tmp/ziman_regex_test

test-handles: src/xalloc.c src/handles.c tests/c/handles_test.c
	$(CC) $(PLAIN_CFLAGS) -Wl,--wrap=malloc -Wl,--wrap=calloc -o /tmp/ziman_handles_test src/xalloc.c src/handles.c tests/c/handles_test.c
	/tmp/ziman_handles_test

test-decimal: src/xalloc.c src/bignum.c src/decimal.c tests/c/decimal_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_decimal_test src/xalloc.c src/bignum.c src/decimal.c tests/c/decimal_test.c
	/tmp/ziman_decimal_test

# src/arena.c doesn't need the GC/plain distinction (it's built on mmap/munmap directly, never
# malloc/free, precisely so it stays inert under either -- see its own header comment), but is
# tested the same standalone way as the other C modules above rather than through the interpreter.
# src/coverage.c alone: the lcov writer and, mainly, that writing into a file which already has
# records adds to them rather than replacing them. The AST is built by hand in the test, so nothing
# else links.
test-coverage: src/xalloc.c src/coverage.c tests/c/coverage_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_coverage_test src/xalloc.c src/coverage.c tests/c/coverage_test.c
	/tmp/ziman_coverage_test

test-arena: src/xalloc.c src/arena.c tests/c/arena_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_arena_test src/xalloc.c src/arena.c tests/c/arena_test.c
	/tmp/ziman_arena_test

# src/gc_object.c sits on top of src/arena.c (Phase 3's object-header layer, docs/MEMORY.md) --
# same standalone-test convention as arena.c itself, ahead of any mark/copy logic built on it.
test-gc-object: src/xalloc.c src/arena.c src/gc_object.c tests/c/gc_object_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_object_test src/xalloc.c src/arena.c src/gc_object.c tests/c/gc_object_test.c
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
test-gc-copy: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_copy.c tests/c/gc_copy_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_copy_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_copy.c tests/c/gc_copy_test.c
	/tmp/ziman_gc_copy_test

# src/gc_lookup.c resolves a bare address (a future conservative stack scan's own candidate words,
# docs/MEMORY.md Phase 3) to the gc_alloc'd object it falls within or inside, by walking arena.c's
# own block layout via gc_object.c's gc_next_object -- no interpreter Value machinery needed, so
# this stays as dependency-free as arena.c/gc_object.c's own tests, unlike gc_copy.c's.
test-gc-lookup: src/xalloc.c src/arena.c src/gc_object.c src/gc_lookup.c tests/c/gc_lookup_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_lookup_test src/xalloc.c src/arena.c src/gc_object.c src/gc_lookup.c tests/c/gc_lookup_test.c
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
test-gc-scan: src/xalloc.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/stack.c src/error.c tests/c/gc_scan_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_scan_test src/xalloc.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/stack.c src/error.c tests/c/gc_scan_test.c
	/tmp/ziman_gc_scan_test

# src/gc_heap.c is Phase 3's actual collected heap -- a free-list-capable allocator on top of
# arena.c/gc_object.c, needed once Phase 3 switched from a copying design to mark-sweep (a
# conservative stack scan's own roots can never safely be relocated; see gc_heap.h's own header
# comment for why, and docs/MEMORY.md for the fuller story). gc_heap_collect wires gc_scan.c and
# gc_mark.c together with gc_heap.c's own alloc/sweep, so this now needs their full dependency
# chain too (real ArrayObj/StructObj/etc. construction for gc_mark.c, stack.c/error.c for
# gc_scan.c), not just arena.c/gc_object.c on their own.
test-gc-heap: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_env_roots.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/c/gc_heap_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_heap_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_env_roots.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/c/gc_heap_test.c
	/tmp/ziman_gc_heap_test

# src/gc_mark.c is the mark phase mark-sweep needs: walks a real Value graph by type.kind (mirroring
# gc_copy.c's own per-kind structure) and calls gc_set_marked instead of copying. Needs the same
# dependency chain as gc_copy_test.c's own (real ArrayObj/StructObj/etc. construction), plus
# gc_lookup.c (interior-pointer resolution for string views and array slices during marking).
# value.c's own value_string_copy now allocates through gc_thread_heap.c/gc_heap.c, which pulls in
# gc_scan.c transitively too.
test-gc-mark: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/c/gc_mark_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_mark_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c tests/c/gc_mark_test.c
	/tmp/ziman_gc_mark_test

# src/gc_env_roots.c is the second root source a real collection cycle needs besides the
# conservative stack scan: an Env (env.h) lives in the ordinary allocator's own heap, never a
# thread's GcHeap arena, so once a Value is stored into a binding its own pointer fields move off
# the C stack (where gc_scan_stack_roots could see them) into Env.items' own heap storage --
# invisible to a scan that only reads raw stack bytes and never dereferences what it finds. Every
# ordinary variable is affected, not just interpreter.c's t_defer_envs/t_env_pool, which is where
# this gap was originally (too narrowly) expected to live -- see gc_env_roots.h's own header
# comment and docs/MEMORY.md for the fuller story.
test-gc-env-roots: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c tests/c/gc_env_roots_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_env_roots_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c tests/c/gc_env_roots_test.c
	/tmp/ziman_gc_env_roots_test

# src/gc_interp_roots.c aggregates every root source a real collection cycle needs beyond the
# conservative stack scan: the global scope, every active call's own frame (via error.c's frame
# tracking), and interpreter.c's own t_defer_envs/t_env_pool -- taken as explicit parameters rather
# than read from interpreter.c's private state directly, since interpreter.c is too large to link
# into a standalone test the way every other module here has been. This test builds synthetic
# stand-ins for that state, exercising the identical code path a real interpreter would.
test-gc-interp-roots: src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c src/gc_interp_roots.c tests/c/gc_interp_roots_test.c
	$(CC) $(PLAIN_CFLAGS) -o /tmp/ziman_gc_interp_roots_test src/xalloc.c src/ast.c src/error.c src/stack.c src/bignum.c src/decimal.c src/mapobj.c src/value.c src/value_copy.c src/env.c src/arena.c src/gc_object.c src/gc_lookup.c src/gc_scan.c src/gc_mark.c src/gc_heap.c src/profile.c src/gc_thread_heap.c src/gc_env_roots.c src/gc_interp_roots.c tests/c/gc_interp_roots_test.c
	/tmp/ziman_gc_interp_roots_test

# ./ziman-allocprofile: the collector build plus a count of every allocation against its source
# site, dumped to stderr at exit, hottest first. Not a normal build and not fast -- it takes a mutex
# per allocation -- but it answers "what actually allocates" exactly rather than by sampling, which is
# how F7's three hottest sites were found (the review had named two different ones). See
# src/allocsites.c. Run as: ./ziman-allocprofile <program.zm> 2>&1 | sed -n '/allocations by site/,$p'
ziman-allocprofile: $(SRC) $(HDR) $(CORELIB_HEADERS) $(GC_LIB)
	$(CC) $(PLAIN_CFLAGS) -DZIMAN_VERSION='"$(ZIMAN_VERSION)-allocprofile"' $(GC_FLAGS) 	    -DZIMAN_ALLOC_PROFILE -o $@ $(SRC) $(GC_LIB) -lm -lsqlite3 -lssl -lcrypto -lcrypt -lz -pthread

asan: $(SRC) $(CORELIB_HEADERS) $(GC_LIB)
	$(call san_link,asan,,-fwrapv -fsanitize=address,libasan,,)
	$(call san_link,asan,-gc,-fwrapv -fsanitize=address,libasan,$(GC_FLAGS),$(GC_LIB))
	@for bin in ziman-asan ziman-asan-gc; do \
	    echo "=== $$bin ==="; \
	    ASAN_OPTIONS=$(ASAN_OPTS) BIN=./$$bin SKIP=stack_depth_deep_recursion_works TIMEOUT=$(SAN_TIMEOUT) \
	        ./tests/run_tests.sh || exit 1; \
	    if command -v openssl >/dev/null 2>&1; then \
	        ASAN_OPTIONS=$(ASAN_OPTS) BIN=./$$bin ./tests/run_tls_tests.sh || exit 1; \
	    else echo "make asan: TLS suite skipped -- openssl is not on PATH"; fi; \
	done

ubsan: $(SRC) $(CORELIB_HEADERS) $(GC_LIB)
	$(call san_link,ubsan,,$(UBSAN_CHECKS),libubsan,,)
	$(call san_link,ubsan,-gc,$(UBSAN_CHECKS),libubsan,$(GC_FLAGS),$(GC_LIB))
	@for bin in ziman-ubsan ziman-ubsan-gc; do \
	    echo "=== $$bin ==="; \
	    UBSAN_OPTIONS=$(UBSAN_OPTS) BIN=./$$bin TIMEOUT=$(SAN_TIMEOUT) ./tests/run_tests.sh || exit 1; \
	    if command -v openssl >/dev/null 2>&1; then \
	        UBSAN_OPTIONS=$(UBSAN_OPTS) BIN=./$$bin ./tests/run_tls_tests.sh || exit 1; \
	    else echo "make ubsan: TLS suite skipped -- openssl is not on PATH"; fi; \
	done

tsan: $(SRC) $(CORELIB_HEADERS) $(GC_LIB)
	$(call san_link,tsan,,-fwrapv -fsanitize=thread,libtsan,,)
	$(call san_link,tsan,-gc,-fwrapv -fsanitize=thread,libtsan,$(GC_FLAGS),$(GC_LIB))
	@for bin in ziman-tsan ziman-tsan-gc; do \
	    echo "=== $$bin ==="; \
	    TSAN_OPTIONS=$(TSAN_OPTS) BIN=./$$bin SKIP=stack_depth_trace_bounded TIMEOUT=$(SAN_TIMEOUT) \
	        ./tests/run_tests.sh || exit 1; \
	    if command -v openssl >/dev/null 2>&1; then \
	        TSAN_OPTIONS=$(TSAN_OPTS) BIN=./$$bin ./tests/run_tls_tests.sh || exit 1; \
	    else echo "make tsan: TLS suite skipped -- openssl is not on PATH"; fi; \
	done

sanitizers: asan ubsan tsan

# memcheck, narrowly -- see tests/run_valgrind.sh for what it covers and why it is not a whole-suite
# sweep, and tools/valgrind.supp for the conservative-scanner suppressions it needs. Built with
# -fno-omit-frame-pointer, without which those suppressions do not match (several contexts unwind
# as `???` and the frame they key on never appears). Not in `make check`: valgrind is about 100x
# slower than ASan. What it buys that the sanitizer targets cannot is uninitialised-value checking,
# which ASan does not do and MSan is not here to do, plus the leak checker that found A26.
ziman-vg: $(SRC) $(HDR) $(CORELIB_HEADERS)
	$(CC) $(PLAIN_CFLAGS) -fno-omit-frame-pointer -DZIMAN_VERSION='"$(ZIMAN_VERSION)-vg"' \
	    -o ziman-vg $(SRC) -lm -lsqlite3 -lssl -lcrypto -lcrypt -lz -pthread

valgrind: ziman-vg test-gc-scan test-gc-lookup test-arena test-gc-object test-gc-interp-roots test-handles test-coverage test-regex
	./tests/run_valgrind.sh ./ziman-vg

# GCC's static analyzer over both configurations. Clean as of 2026-10-08 and deliberately not part of
# `make check`: it takes minutes per configuration, where every other check here takes seconds.
analyze:
	$(CC) -std=c11 -O1 -fwrapv -pthread -I$(BUILD) -fanalyzer -fsyntax-only $(GC_FLAGS) $(SRC)
	$(CC) -std=c11 -O1 -fwrapv -pthread -fanalyzer -fsyntax-only $(SRC)
	@echo "analyze: -fanalyzer clean on both configurations"

libc-free-check:
	python3 tools/check_libc_free.py

xalloc-check:
	python3 tools/check_xalloc.py

# A HandleSet without HANDLE_SET_INIT cannot fail a test on a machine whose libc zero-fills
# PTHREAD_MUTEX_INITIALIZER, which is every glibc machine -- so only a check catches it (A45).
handleset-check:
	python3 tools/check_handle_sets.py
