# make/docs.mk -- generating the documentation and checking it has not drifted.
#
# docs/lib/ and docs/CORELIB.md are generated from lib/'s own declarations, docs/DESIGN.md carries
# a generated section index, and the rest of these read the hand-written pages looking for a claim
# the sources no longer support -- a module that was deleted, a "not yet" that shipped, an example
# that stopped compiling.
#
# Variables live in the top-level Makefile, never here.

# docs/CORELIB.md and the manual in docs/lib/ are generated from the lib/core and lib/db modules' own `pub` declarations
# (see tools/gen_corelib_reference.sh) -- not run as part of `make`/`make
# all` since it's a doc, not a build input; run by hand after touching
# any core module's public API. The generated file is still checked into
# git (unlike build/*_source.h) since it's meant to be read on GitHub, not
# just consumed by the build.
corelib-reference:
	./tools/gen_corelib_reference.sh > docs/CORELIB.md
	python3 tools/gen_lib_manual.py
	python3 tools/gen_lib_deps.py

# Fails when docs/CORELIB.md is not what the current lib/ sources generate (run
# `make corelib-reference`). Added in F167, which found the committed file already wrong: it had
# the `schedule` section before `sqlite` and `server`, where the generator emits `lib/time/*.zm`
# last. `lib-manual-check` below diffs `docs/lib` only, so the other generated page had no gate at
# all and nobody could have noticed -- the same hole, in the same shape, as `src/lsp.c`'s
# hand-kept builtin list that the same task replaced.
corelib-reference-check:
	./tools/gen_corelib_reference.sh > docs/CORELIB.md
	git diff --exit-code --stat -- docs/CORELIB.md

# Fails when docs/lib is not what the current lib/ sources generate (run `make corelib-reference`).
lib-manual-check: $(BIN)
	python3 tools/gen_lib_manual.py
	python3 tools/gen_lib_deps.py
	git diff --exit-code --stat -- docs/lib

# Fails when `tools/gen_lib_map.py` cannot run.
#
# **It is here because nothing ran it.** The script raised `KeyError: 5` for an unknown length of
# time -- its layer-label table was written for a four-layer lib/ and the sources grew to six --
# and no target noticed, because its output is for a website that does not exist yet and is not
# committed, so the usual `git diff --exit-code` shape does not apply. Running it and throwing the
# output away is the whole check: a generator that crashes is the only failure it can have, and
# that is exactly the failure it had.
lib-map-check:
	@python3 tools/gen_lib_map.py $(shell mktemp -t ziman-lib-map-XXXXXX.html) >/dev/null
	@echo "ok   gen_lib_map.py runs"

# Fails when the editor grammar stops giving a token the scope it is meant to. The grammar had no
# test of any kind until 2026-10-12 -- nothing loaded it, nothing asserted a scope -- and three real
# gaps sat in it unnoticed (`internal` matched nothing, `#![...]` was not an attribute, `val`/`var`
# shared one scope). It needs no binary: the grammar is data and this reads it.
grammar-check:
	python3 tools/check_grammar.py

# Fails when docs/SYNTAX.md or a lib/**/*.zm doc comment claims some `pub` name is "not yet"
# supported when it's already declared -- see tools/check_not_yet.py's own header comment.
not-yet-check:
	python3 tools/check_not_yet.py

# Fails when README.md's module table disagrees with the modules' own `#![experimental]`
# attribute -- see tools/check_experimental.py's own header comment.
experimental-check:
	python3 tools/check_experimental.py

# Fails when README.md, docs/TUTORIAL.md, docs/SYNTAX.md or docs/CHEATSHEET.md names a module that
# lib/ no longer has, or a function a module no longer exports -- see tools/check_doc_modules.py's
# own header comment. The generated docs (docs/lib/, docs/CORELIB.md) cannot go stale; these four
# can, and F138 was one that did, for months, in the first file a newcomer reads.
doc-module-check:
	python3 tools/check_doc_modules.py

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

# The editor grammar, generated like the manuals rather than hand-kept. On 2026-10-19 its lists
# still named `ArrayBuilder`, `sb_append`, `mapb_set` and `concat_arrays` -- all deleted during
# F149-F162 -- and knew none of the 28 names F178-F180 unprefixed. Nothing was watching, which is
# the same hole `src/lsp.c` had before F168 and the reason both are now derived.
vscode-grammar:
	python3 tools/gen_vscode_grammar.py $(BIN)

vscode-grammar-check: $(BIN)
	python3 tools/gen_vscode_grammar.py $(BIN)
	git diff --exit-code -- editors/vscode/syntaxes/ziman.tmLanguage.json

# The grammar's name lists are derived, but `showcase.zm` -- the file the extension's README sends a
# reader to in order to eyeball a highlighting change -- was hand-written and compiled by nothing.
# By 2026-10-21 it called `set_union` and `map_get`, renamed to `union` and `get` by F195-F201, so
# the demonstration of the language named two functions the language does not have. Compiling it is
# the whole guard, and it catches the next rename for free. `--fmt --check` as well, since a file
# shown off as exemplary should be formatted the way the formatter formats it.
#
# This checks `models/user.zm` too, which showcase.zm `use`s and which has no `main` of its own.
showcase-check: $(BIN)
	$(BIN) --check editors/vscode/showcase.zm
	$(BIN) --fmt --check editors/vscode/showcase.zm

docs-check: lib-manual-check corelib-reference-check not-yet-check experimental-check doc-module-check syntax-check design-index-check vscode-grammar-check showcase-check lib-map-check
