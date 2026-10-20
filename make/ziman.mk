# make/ziman.mk -- running Ziman: the language suites, the standard library, and its tools.
#
# Every target here runs the built binary over Ziman source: the language fixtures under
# tests/lang/, the stdlib #[test] units under tests/stdlib/, the integration suites that need a
# database or OpenSSL, the linter, the coverage report and the formatter.
#
# Variables live in the top-level Makefile, never here.

# The `#[test]` units for the bundled modules, under tests/stdlib/. There were **zero** `#[test]`
# functions in 50,000 lines of Ziman until F134, in a language that ships the attribute, the runner
# and an assertion module -- C13 removed the stated reason (an unreachable sibling test directory)
# and nothing followed. The argument for them is not principle but two defects: B12 (a missing
# `ORDER BY`) and E25 (an unpadded fraction in a sort key) both live below the level any end-to-end
# check can see, and both would have been one assertion here.
#
# They live outside lib/ on purpose. Bundling is driven by the explicit CORELIB_MODULES list, so a
# test file in lib/ would not be embedded -- but docs/lib generation, `make lint` and
# `make lib-coverage` all glob lib/**/*.zm and would each need an exception.
lib-tests: $(BIN)
	@./$(BIN) test tests/stdlib/*.zm

test-nogc: ziman-nogc
	BIN=./ziman-nogc ./tests/run_tests.sh

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
# (tests/lang is not: fixtures that test error positions depend on their exact layout).
#
# This repo only. The sample backend lives in a sibling checkout and has its own target below --
# it used to be in this one, which made a gate on *this* repo fail for a file in another, and meant
# anyone without that checkout could not run `make fmt-check` at all. Found when `make check` first
# ran the whole set together: it had been failing on three of the backend's files.
# `--canonical` as well as plain `--fmt`, added 2026-10-12 (docs/reviews/tasks.md F80). Both corpora
# already passed it, so this changed no file -- what it changes is that they are now *held* to it
# rather than happening to agree, and that the flag is exercised by something. The consequence worth
# naming: `--canonical` settles brace placement, so this repo now requires K&R braces where plain
# `--fmt` would also accept Allman.
fmt-check: $(BIN)
	./$(BIN) --fmt --check $$(find lib docs/lib/examples docs/reviews/benchmark/reference tests/integration/pg tests/integration/mysql tests/integration/tls tests/integration/shutdown tests/bench -name '*.zm')
	./$(BIN) --fmt --canonical --check $$(find lib docs/lib/examples docs/reviews/benchmark/reference tests/integration/pg tests/integration/mysql tests/integration/tls tests/integration/shutdown tests/bench -name '*.zm')
fmt-check-backend: $(BIN)
	@if [ -d "$(BACKEND_DIR)/src" ]; then \
	    ./$(BIN) --fmt --check $$(find $(BACKEND_DIR)/src -name '*.zm') && \
	    ./$(BIN) --fmt --canonical --check $$(find $(BACKEND_DIR)/src -name '*.zm'); \
	else \
	    echo "SKIP fmt-check-backend: $(BACKEND_DIR)/src is not checked out"; \
	fi

# In `make check` since 2026-10-12. It was not, which is how a `len(result) >= 5` assertion on
# document highlight survived while the handler answered with every word that matched the text:
# nothing in the gate ran the file that would have been strengthened (docs/reviews/tasks.md F77).
test-lsp: $(BIN)
	python3 tests/integration/lsp/lsp_test.py ./$(BIN)

test-tls: $(BIN)
	./tests/run_tls_tests.sh

# The TLS suite as part of `make check`, when it can run. It needs `openssl` on PATH -- the harness
# generates certificates with it and starts two `openssl s_server` processes -- which is why it was
# left out of `check` in the first place. Leaving it out had a cost: **no TLS code path in this
# project had ever run under a sanitizer**, and that is how a use-after-free on an `SSL` object
# survived A24, a review specifically about races in that file (docs/reviews/tasks.md A36, and A32
# for the bug). Skipped with a word rather than silently, so a machine without openssl still gets a
# green `check` and knows what it did not run.
# Type-checks and runs the North Star application -- the slice of the health-record backend
# docs/ROADMAP.md's first section names as the target this language exists to reach. It lives in a
# sibling checkout, so this follows tls-if-available: run it when it is there, say so when it is
# not, never fail for its absence. It had been broken for some time before this existed
# (docs/reviews/tasks.md F25), which is the argument for having it: nothing here was watching the
# one application that defines "capable enough".
# How `use` finds a module that is not beside the importing file: ZIMAN_MODULE_PATH, and a
# `ziman.toml` marking the project root. A script rather than a fixture because the entry file has
# to sit *below* the root, which tests/run_tests.sh's `name/main.zm` layout cannot express -- see
# the script's own header.
# `ziman --api`, which answers "what does this module offer" -- the question the 2026-10-09
# generation benchmark measured as its largest cost. A script, not a fixture: it is a command that
# takes no program.
api-check: $(BIN)
	@./tests/run_api.sh $(BIN)
	python3 tools/check_builtins.py

module-path-check: $(BIN)
	@./tests/run_module_path.sh $(BIN)

# Stage 1 of docs/design/full-overloading.md. The new resolver runs beside the hand-written
# dispatch arms over every Ziman file in reach -- fixtures, the benchmark references, lib/, and
# the North Star when it is there -- and this fails if the two ever choose a different world.
# Gated from the first stage on purpose: the resolver decides nothing yet, so the only thing that
# can tell us it is right is the corpus, and the only thing that keeps it right while the arms are
# removed one by one is a gate that was there before the removals started.
overload-shadow-check: $(BIN)
	@./tools/overload_shadow.sh $(BIN)

northstar-if-available: $(BIN)
	@./tests/run_northstar.sh $(BIN)

northstar-both: $(BIN)
	@set -e; app=$${ZIMAN_NORTHSTAR:-../health-record/health-record-backend-ziman}; 	if [ ! -x "$$app/src/test/other/run-all.sh" ]; then 	    echo "northstar-both: no North Star checkout at $$app (set ZIMAN_NORTHSTAR)"; exit 1; 	fi; 	cd "$$app/src/test/other" && ZIMAN=$(abspath $(BIN)) ./run-all.sh --both

# Both builds, because the TLS suite ran only against $(BIN) and that is exactly how A39 survived:
# two `value_string` calls handed a borrowed pointer to a stack array, which the default build
# happened to read back intact and `ziman-nogc` at -O2 did not -- `peer_certificate_subject`
# returned "" for every certificate there, 3 of 3 runs, while the gate stayed green. The whole
# suite takes under a second, so running it twice is the cheapest of the three gaps of this shape
# this file has now grown a fix for (see B7's --postgres job and E21's --fmt --check).
# Poppler's pdfinfo/pdftotext checking lib/graphics' PDF output -- an independent reader agreeing,
# which `pdfobj.check` cannot be: that is this project's reader agreeing with its own writer, and a
# malformed file can be perfectly self-consistent. Follows tls-if-available: run it when the tools
# are there, say so when they are not, never fail for their absence.
pdf-if-available: $(BIN)
	@if command -v pdfinfo >/dev/null 2>&1 && command -v pdftotext >/dev/null 2>&1; then \
	    ./tests/run_pdf_tests.sh $(BIN); \
	else \
	    echo "pdf-if-available: skipped -- no pdfinfo/pdftotext (poppler-utils)"; \
	fi

tls-if-available: $(BIN) ziman-nogc
	@if command -v openssl >/dev/null 2>&1; then \
	    ./tests/run_tls_tests.sh && BIN=./ziman-nogc ./tests/run_tls_tests.sh; \
	else \
	    echo "tls-if-available: skipped -- openssl is not on PATH, so the TLS suite cannot run"; \
	fi

# Fails when the generation benchmark's own reference solutions stop compiling or stop printing what
# their task says they print. The benchmark is 50 task prompts with expected outputs
# (docs/reviews/benchmark); an `expected.txt` records what Ziman did on the day it was written, not
# what it must do forever, so a failure here means a language or library change has moved under the
# task set and the *tasks* need revisiting. Half a second, because it is 50 small programs.
benchmark-check: $(BIN)
	@./docs/reviews/benchmark/run.sh docs/reviews/benchmark/reference ./$(BIN) | tail -1
	@./docs/reviews/benchmark/run.sh docs/reviews/benchmark/reference ./$(BIN) | grep -q "50 compiled, 50 correct, 0 missing" \
	    || { echo "benchmark-check: the reference solutions no longer match their expected output"; exit 1; }

# `--lint` over every library module. Two things had to be true before this could exist at all,
# and neither was (docs/reviews/tasks.md F133).
#
# `--lint` refused a module with no `main`, so it could only ever be pointed at an entry point.
# And a module reached through `use` is normally the *bundled* copy, whose declarations carry a
# NULL `source_file` -- which is exactly the flag the lint uses to tell "your code" from the
# standard library, so every finding in `lib/` was discarded before it was printed. Pointing
# ZIMAN_MODULE_PATH at the sources makes them load from disk with real paths, and the findings
# appear.
#
# What that was hiding: the `spawn` rule's own message is "whoever is waiting on that channel
# waits for ever", which is A47 word for word, and it could not see the two `spawn serve` calls
# that had it.
#
# NOT in `check` yet, deliberately. Three findings remain and all three are quadratic-accumulation
# warnings on loops that are bounded in practice: `time.zm:280` accumulates the few literal
# characters between two tokens of a format pattern and says so in a comment that predates this
# target, and `yaml.zm:698`/`:709` have predicates (`flow_depth`, `quote_open`) that rescan the
# accumulation every round, so a builder would snapshot per round and cost the same -- making
# those linear needs the depth and quote state tracked incrementally. Gating needs either those
# three resolved or a way to record "bounded, and here is why" in the source, the way
# `xalloc-ok:` does for check_xalloc.py.
lint: $(BIN)
	@python3 tools/lint_lib.py

# A line-coverage figure for lib/, merged across the whole fixture suite. Not in `check`: it runs
# every fixture again under --coverage and takes a few minutes, which is a reporting job rather
# than a gate. See tools/lib_coverage.py on why `lib/` was invisible to --coverage until now
# (the same bundled-module blindness as F133's) and why the drivers are reported separately.
lib-coverage: $(BIN)
	@python3 tools/lib_coverage.py
