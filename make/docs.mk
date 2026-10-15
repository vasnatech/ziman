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

# Fails when docs/lib is not what the current lib/ sources generate (run `make corelib-reference`).
lib-manual-check: $(BIN)
	python3 tools/gen_lib_manual.py
	python3 tools/gen_lib_deps.py
	git diff --exit-code --stat -- docs/lib

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

docs-check: lib-manual-check not-yet-check experimental-check doc-module-check syntax-check design-index-check
