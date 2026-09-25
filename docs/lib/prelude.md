# `prelude`

Every type/function/val declared here is merged bare,
unqualified, into every Ziman program, no `use` required (see
docs/DESIGN.md's "Prelude" entry). Deliberately *not* a `use`-able
core module despite living in lib/core/ alongside ones that are: its
generated header is still produced by the ordinary corelib Makefile
rule (real .zm source, no hand-escaped C string, same as every other
bundled module), but it's never added to corelib.c's CORE_MODULES
table, so `use prelude` doesn't resolve to it -- there would be no
reason to `use` something already available everywhere, and doing so
would just parse this file a second time under a redundant qualifier.

`pub` is meaningless here and never written -- privacy is enforced by
the loader's cross-module resolution (loader.c's resolve_named_imports/
resolve_cross_refs), which this file's contents never go through at
all (see loader.c's load_module_graph: parsed once, merged directly,
no self-qualify pass, same treatment the entry module's own bare
names get).

One native fn declaration per line, no body -- exactly the shape
io.zm uses for a real native module. Their C implementations are
registered by prelude_register() in src/prelude.c.

*This module is never imported: everything it declares is available bare in every program.*
