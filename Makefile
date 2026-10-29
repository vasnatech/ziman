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

# The options the collector itself is compiled with, named once because there is now more than one
# build of it (see GC_LIB_TSAN).
GC_BUILD_FLAGS := -O2 -DNDEBUG -I$(GC_DIR)/include -DGC_THREADS -DGC_BUILTIN_ATOMIC \
    -DTHREAD_LOCAL_ALLOC -DPARALLEL_MARK -DUSE_MMAP -DALL_INTERIOR_POINTERS -D_GNU_SOURCE

# A second copy of the collector, instrumented, for `make tsan`'s `-gc` configuration.
#
# Why it cannot share $(GC_LIB): bdwgc has its own ThreadSanitizer adaptations (see
# include/private/gc_locks.h and gc_atomic_ops.h, guarded on `THREAD_SANITIZER`), and that macro is
# derived from `__SANITIZE_THREAD__`, which the compiler defines **per translation unit** -- only for
# code it is actually instrumenting. Linking the ordinary $(GC_LIB) into a `-fsanitize=thread`
# interpreter therefore compiled all of them out, so TSan's signal interception met Boehm's
# signal-based stop-the-world unprepared: `GC_stop_world` resends its suspend signals and aborts
# with "Signals delivery fails constantly at GC #N", which failed `spawn_defer_gc_stress` in roughly
# one `make tsan` run in three. See docs/DESIGN.md's entry for the measurement.
GC_LIB_TSAN := $(GC_DIR)/libgc-tsan.a

# -fwrapv: signed overflow in the C code itself is well defined (wraps) instead of
# undefined behaviour the optimiser may exploit. Ziman's own `int` arithmetic
# does not rely on it -- it traps on overflow explicitly (see interpreter.c).
# -Wvla and -Wshadow earn their place; the rest of a much higher warning level was triaged and
# rejected as noise (414 -Wswitch-enum by design, -Wlogical-op on the correct EAGAIN||EWOULDBLOCK
# idiom, and so on -- docs/reviews/tasks.md F14). -Wvla had exactly one hit in the tree and it was
# a real bug: the 96 KB variable-length array inside the regex matcher's recursion (A16).
# Objects, dependency files and the generated corelib headers all live under build/ rather than
# beside the sources. Before 2026-10-14 they sat in src/, so a built tree showed 322 entries there
# for 162 real files and `ls src/` stopped being a useful way to see what the interpreter is made
# of. Nothing outside this file ever referenced one, which is why the move cost nothing: no comment
# and no document cites a .o. `-I$(BUILD)` is in PLAIN_CFLAGS because corelib.c says
# `#include "json_source.h"`, which used to resolve beside the file and now needs the path.
BUILD := build
PLAIN_CFLAGS := -std=c11 -O2 -fwrapv -Wall -Wextra -Wpedantic -Wvla -Wshadow -g -pthread -I$(BUILD)
CFLAGS := $(PLAIN_CFLAGS) -DZIMAN_VERSION='"$(ZIMAN_VERSION)"' $(GC_FLAGS) -MMD -MP
SRC := $(wildcard src/*.c)
# Every header, as a blunt prerequisite for the targets that compile all of $(SRC) in one `cc`
# invocation (ziman-nogc, ziman-allocprofile, ziman-vg -- see make/c.mk). Those have no depfile to
# track includes with, unlike $(OBJ)'s own -MMD -MP, so listing only $(SRC) left them never
# rebuilding on a header change: `touch src/value.h && make ziman-nogc` answered "up to date",
# which meant `make test-nogc` -- and so `make check-fast` -- could validate a stale binary after a
# change to a struct layout, an inline function or a macro. `struct Value` itself lives in a header.
# Blunt rather than precise on purpose: those recipes recompile every file anyway, so per-file
# accuracy would buy nothing.
HDR := $(wildcard src/*.h)
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)
BIN := ziman

# Stated rather than left to target order. `all` lives in make/c.mk, which is included at the foot
# of this file, so the first target make sees here would otherwise be `check-fast` -- and a bare
# `make` would run the gate instead of building the compiler. Caught by comparing .DEFAULT_GOAL
# against the pre-split Makefile; nothing else would have noticed until someone typed `make`.
.DEFAULT_GOAL := all

.PHONY: all check check-fast lint lib-tests lib-coverage install-hooks unit-tests clean fmt-check fmt-check-backend docs-check experimental-check doc-module-check test test-nogc nogc test-pg test-mysql test-tls test-lsp test-bignum test-decimal test-handles test-arena test-coverage test-gc-object test-gc-copy test-gc-lookup test-gc-scan test-gc-heap test-gc-mark test-gc-env-roots test-gc-interp-roots test-maptable test-handleslot corelib-reference corelib-reference-check lib-manual-check not-yet-check syntax-check grammar-check benchmark-check design-index design-index-check lib-map-check template-example-check vscode-grammar vscode-grammar-check libc-free-check xalloc-check tls-if-available pdf-if-available valgrind test-regex asan ubsan tsan sanitizers analyze handleset-check overload-sets-check northstar-if-available module-path-check api-check

# Every gate that needs nothing but this repo and a C toolchain: the fixture suite on both builds,
# the standalone unit tests, formatting, and the documentation guards. One command for "is this
# repo healthy", because the alternative is what actually happened -- `lib-manual-check` sat broken
# on main for a while, since `make test` does not run it and nothing else did either.
#
# Deliberately excluded, since each needs something the repo cannot provide: `test-pg` and
# `test-mysql` (a container runtime and a database image), `test-tls` (the `openssl` binary), and
# the benchmarks under tests/bench (`ab`, and a checkout of the sample backend next door).
# Everything `check` runs except the North Star application suite. 72 seconds measured, against
# over half an hour for that one target alone (49 suites, each starting a real server), which is
# what makes this usable as a pre-push hook -- and `api-check`, the 0.08s target that would have
# caught the red HEAD of A46, is in here.
check-fast: test test-nogc unit-tests api-check module-path-check test-lsp tls-if-available pdf-if-available fmt-check docs-check grammar-check benchmark-check libc-free-check xalloc-check handleset-check overload-sets-check

check: check-fast northstar-if-available

# Points git at tools/hooks, so the hooks are version-controlled instead of living in an
# uncommitted .git/hooks that a fresh clone does not have (A46).
install-hooks:
	git config core.hooksPath tools/hooks
	@echo "install-hooks: core.hooksPath = tools/hooks (pre-push runs check-fast; --no-verify bypasses it)"

# Bundled core modules (see src/corelib.c): each lib/core/<name>.zm file
# is embedded as a C string constant, generated fresh from its .zm
# source every build so the two never drift. Declared as a prerequisite
# of every object file (not just corelib.o) so a from-clean build always
# generates it before anything tries to #include it -- the normal
# -MMD/-MP header tracking only kicks in from the second build onward,
# once a .d file already mentions it. Add a new core module by adding
# its name here and one line to corelib.c's CORE_MODULES table.
CORELIB_MODULES := meta template derive_ord derive_row derive_properties derive_json derive_reflect derive_entity process url fs log metrics regex client random uuid clock time schedule json number math bits encoding thread datasource sqlite mysql memdb sql eql db net crypto pg http server os properties ndjson csv toml yaml xml markdown compress zone mime jwt migrate otp locale test geom shape paint scene pdfobj pdf truetype font image stroker raster layout

# The prelude is not a module and is deliberately NOT in CORELIB_MODULES: it has
# no line in corelib.c's CORE_MODULES table, nothing can `use` it, and
# loader.c's load_module_graph merges every part of it bare into every program
# (see lib/prelude/prelude.zm's own header comment). It is several files rather
# than one so that each is about one type, and a declaration lives in the part
# for the type its first argument has -- so an overload set *is* split when its
# members' types are, and `--api builtins` groups it back together by name.
# Add a part by adding a file in lib/prelude/, its name here, and one line in
# src/prelude.c's PARTS table; removing one is the same three edits, which is
# what retired `collections` on 2026-10-22 once the last of its names had been
# filed by type.
PRELUDE_PARTS := prelude array channel iter map mutable option result set string
# Bundled modules live in lib/core (general-purpose), lib/db (SQL
# drivers) lib/http (http, server, client), lib/time (time, zone, schedule) and lib/graphics (the 2D model and
# its backends); `use` names are flat either way.
vpath %.zm lib lib/core lib/db lib/graphics lib/http lib/time

CORELIB_HEADERS := $(CORELIB_MODULES:%=$(BUILD)/%_source.h) $(PRELUDE_PARTS:%=$(BUILD)/prelude_%_source.h)

-include $(DEP)

# The sample backend (health-record-backend-ziman), which is kept formatted the same way but lives
# next door, so this is separate from `fmt-check` and skips cleanly when it is not checked out.
BACKEND_DIR := ../health-record/health-record-backend-ziman

# The North Star's behavioural suite against BOTH database engines, which `check` deliberately does
# not do: Postgres needs a container runtime, and the first run pulls an image.
#
# Nothing runs this automatically, and there is no CI in this repository or in the backend's -- so
# this target is a named command, not a guard. Said plainly because the gap it covers is real and
# still open: the suite's own header records a schema divergence (TEXT foreign keys onto columns a
# migration had made uuid) that survived two commits, failing 17 of 19 suites on Postgres while
# every SQLite run stayed green. `check` would not have caught it then and will not now.
# Everything worth running before a release that `check` leaves out: the three sanitizers,
# memcheck, the static analyzer, the two-engine behavioural suite and the two database suites. It
# probes for each prerequisite and says what it skipped and why, so "I ran preflight" is checkable
# rather than "I ran the parts that happened to work" -- see tools/preflight.sh's own header for why
# this exists at all. Still a command a person runs: there is no CI here.
preflight: $(BIN)
	@./tools/preflight.sh

# Every guard that keeps the documentation from drifting from the sources it describes, in one
# target, because each of these is only useful if something actually runs it: `lib-manual-check`
# was failing on main for a while precisely because nothing did.
# ---------------------------------------------------------------- sanitizers
#
# One target per sanitizer. Each builds **both** configurations -- the plain one and the collector
# one -- and runs the whole fixture suite against each, because a sanitizer only sees the code the
# preprocessor left in the translation unit it compiled.
#
# Until 2026-10-14 all three built the plain configuration only, and the comment here told you to
# add $(GC_FLAGS) and $(GC_LIB) by hand for the other one. Nobody did. That is the whole of
# docs/reviews/tasks.md F123, and the size of the hole is measurable: 218 non-blank lines across 17
# files sit inside `#ifdef ZIMAN_GC` and were never once compiled by these targets, the largest
# being gc_boehm_roots.c (47), arena.c (32) and native_sqlite.c (29). Both of the concurrency bugs
# the last review raised are in there -- A41 in native_net.c and A42's finalizer deadlock in
# native_sqlite.c -- so "clean under the sanitizers" described a configuration nobody ships and said
# nothing about the one everybody runs. Beyond the #ifdefs, src/zgc.h redirects malloc/calloc/
# realloc/strdup to the collector and makes `free` a no-op program-wide, so ASan's allocator
# interceptors see a different program in the two builds even where the source is identical.
#
# The two builds are now one `$(call san_link,...)` apart in the same recipe, so the next person to
# change one cannot leave the other behind -- exactly the failure mode I2 found three times over
# (A43, F130, F131: each the untouched sibling of a fix).
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
# all six builds are green on this tree. That is the argument for having them: nothing else here
# could have found either.
#
# The runtime libraries are packaged separately on some distributions (libasan, libubsan, libtsan on
# Fedora) and were not installed on the machine these targets were written on, so each recipe passes
# $(LDFLAGS) through: a runtime unpacked elsewhere can be used without editing this file, and a link
# that fails for that reason says so instead of printing a wall of ld output. That path is not
# hypothetical -- it is how all six builds below were first run here, from an `rpm2cpio` of the
# Fedora packages into a scratch directory with `libasan.so` symlinked to `libasan.so.8.0.0`:
#
#     make asan LDFLAGS="-L/path/to/lib -Wl,-rpath,/path/to/lib"
#
# gcc's own libasan_preinit.o ships with the compiler, so the shared objects are the only part
# `dnf download libasan libubsan libtsan` has to supply, and no root is needed.
SAN_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Wpedantic -fno-omit-frame-pointer -pthread -I$(BUILD)
SAN_LIBS := -lm -lsqlite3 -lssl -lcrypto -lcrypt -lz -pthread
# Deliberately without -fwrapv, which the ordinary build carries: with it, signed overflow is
# defined and UBSan's integer checks have nothing to say. This is the build that found A21.
UBSAN_CHECKS := -fsanitize=undefined,signed-integer-overflow,float-divide-by-zero,float-cast-overflow,bounds-strict,pointer-overflow,unreachable

# $(call san_link,<target>,<variant suffix>,<-fsanitize flags>,<runtime package>,<extra cflags>,<extra link inputs>)
# Links ziman-<target><suffix>. The suffix is empty for the plain configuration and `-gc` for the
# collector one; everything else about the two builds is identical, which is the point.
define san_link
@$(CC) $(SAN_CFLAGS) $(5) $(3) -DZIMAN_VERSION='"$(ZIMAN_VERSION)-$(1)$(2)"' \
    -o ziman-$(1)$(2) $(SRC) $(LDFLAGS) $(6) $(SAN_LIBS) 2> .$(1)$(2)-link.log \
    || { cat .$(1)$(2)-link.log; rm -f .$(1)$(2)-link.log; \
         echo "make $(1): that link needs the $(4) runtime, which this machine may not have"; \
         echo "  install it (Fedora: dnf install $(4)), or point at an unpacked copy:"; \
         echo "  make $(1) LDFLAGS=\"-L/path/to/lib -Wl,-rpath,/path/to/lib\""; exit 1; }
@rm -f .$(1)$(2)-link.log
endef

ASAN_OPTS := detect_leaks=0:detect_stack_use_after_return=0:halt_on_error=1
UBSAN_OPTS := print_stacktrace=1:halt_on_error=1
# `suppressions` covers one known-safe pattern Boehm's stop-the-world marking creates and TSan
# cannot see as synchronisation -- tools/tsan.supp says which and why. Without it, every run
# with the collector reports the same race on `spawn_defer_gc_stress`.
# What a fixture gets under a sanitizer before the harness calls it a deadlock. The suite's own
# default is five seconds, which is right for an ordinary build and too tight for an
# instrumented one -- see tests/run_tests.sh's own comment on TIMEOUT.
SAN_TIMEOUT := 30

TSAN_OPTS := halt_on_error=1:history_size=4:suppressions=tools/tsan.supp

clean:
	rm -f $(BIN) ziman-nogc ziman-allocprofile ziman-asan ziman-ubsan ziman-tsan ziman-vg \
	    ziman-asan-gc ziman-ubsan-gc ziman-tsan-gc $(GC_DIR)/gc.o $(GC_LIB) \
	    $(GC_DIR)/gc-tsan.o $(GC_LIB_TSAN)
	rm -rf $(BUILD)
# Generated headers from before they moved into build/. Harmless to remove when absent, and
# actively worth removing when present: a quoted #include finds the stale one beside corelib.c
# first, so a tree that still has them builds the old standard library without saying so.
	rm -f src/*_source.h src/*.o src/*.d

# ---------------------------------------------------------------- the three worlds
#
# Split out of this file on 2026-10-14. It had 61 targets in one place and no signal about which
# of them built C, which ran Ziman and which wrote documentation. The variables all stay above, so
# an included file can never be parsed before the thing it reads exists.
include make/c.mk
include make/ziman.mk
include make/docs.mk
