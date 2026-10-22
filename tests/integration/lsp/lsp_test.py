#!/usr/bin/env python3
"""Drives `ziman --lsp` over stdio and checks diagnostics, formatting, symbols,
definition, hover, completion and shutdown."""
import json, os, subprocess, sys, tempfile

ZIMAN = sys.argv[1] if len(sys.argv) > 1 else "./ziman"
cache = tempfile.mkdtemp()
proc = subprocess.Popen([ZIMAN, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        env=dict(os.environ, XDG_CACHE_HOME=cache))
_id = 0
pending = []

def send(obj):
    body = json.dumps(obj).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()

def read():
    n = 0
    while True:
        line = proc.stdout.readline()
        if not line:
            raise SystemExit("server closed the pipe")
        if line in (b"\r\n", b"\n"):
            break
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":")[1])
    return json.loads(proc.stdout.read(n))

def request(method, params):
    global _id
    _id += 1
    send({"jsonrpc": "2.0", "id": _id, "method": method, "params": params})
    while True:
        m = read()
        if m.get("id") == _id:
            return m
        pending.append(m)

def notify(method, params):
    send({"jsonrpc": "2.0", "method": method, "params": params})

def diagnostics_for(uri):
    while True:
        for i, m in enumerate(pending):
            if m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri:
                del pending[i]
                return m["params"]["diagnostics"]
        pending.append(read())

failures = 0
def check(name, cond, detail=""):
    global failures
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  " + str(detail)))
    if not cond:
        failures += 1

tmp = tempfile.mkdtemp()
main_path = os.path.join(tmp, "main.zm")
util_path = os.path.join(tmp, "util.zm")
main_uri, util_uri = "file://" + main_path, "file://" + util_path

init = request("initialize", {"processId": None, "rootUri": "file://" + tmp, "capabilities": {}})
check("initialize advertises capabilities", init["result"]["capabilities"]["definitionProvider"])
notify("initialized", {})

util_src = ('pub struct Point {\n    x: int\n    y: int\n}\n\npub fn double(n: int) -> int {\n    return n * 2\n}\n'
            '\npub fn total(ns: variadic int) -> int {\n    var sum = 0\n    for n in ns { sum = sum + n }\n    return sum\n}\n')
main_bad = 'use util\n\nfn main() {\n    val a: int = "no"\n    print(util.double(a))\n    print(missing)\n}\n'
notify("textDocument/didOpen", {"textDocument": {"uri": util_uri, "languageId": "ziman", "version": 1, "text": util_src}})
diagnostics_for(util_uri)
notify("textDocument/didOpen", {"textDocument": {"uri": main_uri, "languageId": "ziman", "version": 1, "text": main_bad}})
d = diagnostics_for(main_uri)
check("two errors reported at once", len(d) == 2, d)
check("first error on line 4", d and d[0]["range"]["start"]["line"] == 3, d)

main_ok = 'use util\n\nfn main() {\n    val a = 21\n    print(util.double(a))\n    print(util.total(1, 2))\n}\n'
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 2}, "contentChanges": [{"text": main_ok}]})
check("fixing the buffer clears diagnostics", diagnostics_for(main_uri) == [])

r = request("textDocument/definition", {"textDocument": {"uri": main_uri}, "position": {"line": 4, "character": 16}})
check("go to definition crosses files", r["result"] and r["result"]["uri"] == util_uri and r["result"]["range"]["start"]["line"] == 5, r)

r = request("textDocument/hover", {"textDocument": {"uri": main_uri}, "position": {"line": 4, "character": 16}})
check("hover shows the signature", r["result"] and "fn double(n: int) -> int" in r["result"]["contents"]["value"], r)

# The keyword, not the `...` this rendered until 2026-10-12 (docs/reviews/tasks.md F94). Hover is
# how the editor answers "what does a caller write here", which for a variadic parameter is the one
# thing its declared (element) type does not say.
r = request("textDocument/hover", {"textDocument": {"uri": main_uri}, "position": {"line": 5, "character": 16}})
check("hover spells a variadic parameter with the keyword",
      r["result"] and "fn total(ns: variadic int) -> int" in r["result"]["contents"]["value"], r)

r = request("textDocument/documentSymbol", {"textDocument": {"uri": util_uri}})
names = sorted(s["name"] for s in r["result"])
check("document symbols", names == ["Point", "double", "total"], names)

r = request("textDocument/completion", {"textDocument": {"uri": main_uri}, "position": {"line": 4, "character": 15}})
labels = [i["label"] for i in r["result"]["items"]]
check("completion after `util.`", "double" in labels and "Point" in labels, labels)

# navigation: `use` lines, aliases, bundled modules, locals, per-name imports
nav = ('use util as u\nuse mime\nuse util.double as twice\n\nfn helper(count: int) -> int {\n    return count\n}\n\n'
       'fn main() {\n    val a = 21\n    for i, item in [a] {\n        print(item + i)\n    }\n'
       '    print(u.double(a))\n    print(mime.essence(mime.text_plain()))\n    print(twice(a))\n    print(helper(a))\n}\n')
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 4}, "contentChanges": [{"text": nav}]})
diagnostics_for(main_uri)

def goto(line, character):
    r = request("textDocument/definition", {"textDocument": {"uri": main_uri}, "position": {"line": line, "character": character}})
    return r["result"]

r = goto(0, 8)
check("ctrl+click on a `use` path opens the module file", r and r["uri"] == util_uri and r["range"]["start"]["line"] == 0, r)
r = goto(1, 6)
check("ctrl+click on a bundled module path opens its cached source", r and r["uri"].endswith("/ziman/core/mime.zm"), r)
r = goto(13, 13)
check("an alias qualifier resolves to the aliased module", r and r["uri"] == util_uri and r["range"]["start"]["line"] == 5, r)
r = goto(13, 11)
check("the alias itself opens the module", r and r["uri"] == util_uri and r["range"]["start"]["line"] == 0, r)
r = goto(14, 18)
check("a call into a bundled module opens its source", r and r["uri"].endswith("/ziman/core/mime.zm") and r["range"]["start"]["line"] > 0, r)
r = goto(15, 11)
check("a name imported with `use mod.Name as alias` resolves", r and r["uri"] == util_uri and r["range"]["start"]["line"] == 5, r)
r = goto(2, 22)
check("ctrl+click on an imported name in its `use` line", r and r["uri"] == util_uri and r["range"]["start"]["line"] == 5, r)
r = goto(15, 17)
check("a local variable goes to its `val`", r and r["uri"] == main_uri and r["range"]["start"]["line"] == 9, r)
r = goto(11, 15)
check("a for-loop binder goes to its `for`", r and r["uri"] == main_uri and r["range"]["start"]["line"] == 10, r)
r = goto(5, 11)
check("a parameter goes to the function header", r and r["uri"] == main_uri and r["range"]["start"]["line"] == 4, r)
r = goto(16, 12)
check("a function of the same file", r and r["uri"] == main_uri and r["range"]["start"]["line"] == 4, r)

r = request("textDocument/hover", {"textDocument": {"uri": main_uri}, "position": {"line": 14, "character": 18}})
check("hover works on bundled modules", r["result"] and "fn essence(" in r["result"]["contents"]["value"], r)
r = request("textDocument/completion", {"textDocument": {"uri": main_uri}, "position": {"line": 14, "character": 17}})
labels = [i["label"] for i in r["result"]["items"]]
check("completion after a bundled module's qualifier", "essence" in labels and "parse" in labels, labels[:10])

# Completion with no qualifier offers the names a program can call with no `use`. Until F167 that
# list was hand-kept in src/lsp.c and nothing tested or gated it, so it had drifted to 88 entries
# against 157 builtins: no `sort`, no `filter`, no `map`, none of the names F162-F167 unprefixed,
# and three that F151 had deleted months earlier. It is now derived from the builtin table and the
# prelude's own declarations, and these four checks are what keeps it so.
r = request("textDocument/completion", {"textDocument": {"uri": main_uri}, "position": {"line": 16, "character": 14}})
labels = [i["label"] for i in r["result"]["items"]]
missing = [n for n in ("sort", "filter", "map", "sort_with", "sort_by", "is_sorted", "binary_search",
                       "fold", "distinct", "narrow", "swap", "mutable") if n not in labels]
check("unqualified completion offers the dispatching builtins", not missing, missing)
# A plain `fn` in lib/prelude.zm is callable with no `use` just as a `builtin fn` is, so the list
# is both sources, not the builtin table alone.
# `bytes_concat` was the first entry here until F213 deleted it, `concat` having always taken
# `variadic [T]`.
missing = [n for n in ("inverse", "filter_present", "filter_ok", "filter_err") if n not in labels]
check("unqualified completion offers the prelude's own functions", not missing, missing)
# The parser's desugaring targets are not names anyone writes.
internal = [n for n in labels if n.startswith("$")]
check("unqualified completion hides the internal builtins", not internal, internal)
# A name that was removed cannot linger here, which is the half of a removal that kept being
# missed: the compiler is happy either way, so only this notices.
gone = [n for n in ("mutable_array_from", "mutable_map_from", "mutable_set_from", "array_join",
                    "sort_by_keys", "mutable_array_truncate", "concat_arrays", "mutable_array_swap",
                    "array_sort_indices", "array_sort_indices_desc", "sort_indices",
                    "array_present", "iter_of_array")
        if n in labels]
check("unqualified completion has dropped the deleted names", not gone, gone)
# F172: `internal` is not deletion -- these five are still callable, and
# tests/lang/prelude_internal_still_callable.zm pins that. They are out of *completion* because
# offering `sort_positions` and `sort_scalars` beside `sort` and `sort_by` is the confusion the
# same change took out of `--api builtins`, and an editor is where it would cost the most.
hidden = [n for n in ("is_sortable_scalars", "sort_scalar_order", "sort_scalars", "permute",
                      "sort_positions") if n in labels]
check("unqualified completion hides the internal prelude natives", not hidden, hidden)
# F173: and what is left of the dispatcher's prefix layer, for the same reason -- completing the
# prefixed spelling offers a worse name for a call the editor could have spelled `add`. This was a
# list of nine until F208; the other eight have no declaration in any spelling any more, so
# naming them here asserted nothing. Only the C symbols still carry the prefix.
hidden = [n for n in ("mutable_set_add",) if n in labels]
check("unqualified completion hides the prefix layer", not hidden, hidden)
# The two with no bare form stay: `mutable_array_new`/`_filled` are constructors, and an editor
# that stopped offering them would leave no way to discover how a builder is made. It was four
# until F209 deleted `mutable_array_get`/`_set`, element access having become an index rather than
# a call the sugar rewrote onto.
missing = [n for n in ("mutable_array_new", "mutable_array_filled") if n not in labels]
check("unqualified completion keeps the prefixed names with no bare form", not missing, missing)
gone = [n for n in ("mutable_array_get", "mutable_array_set") if n in labels]
check("unqualified completion no longer offers the element accesses", not gone, gone)
# The seam a caller would actually want is still there, and so is what it implements.
missing = [n for n in ("sort", "sort_by", "array_order_of") if n not in labels]
check("unqualified completion keeps the sorts the internals implement", not missing, missing)
# The keyword/type half of the list is the one part `src/lsp.c` keeps **by hand** -- this process
# is the compiler and has no `--api builtins` to read -- so it is the part that rots. On
# 2026-10-21 it had `unit` twice, no `internal` (one of exactly three visibility levels, the gap
# the editor grammar closed in F75) and four of the fourteen built-in generic types, **`Option`
# among the ten missing**, which is as written as `Result`.
missing = [n for n in ("Channel", "Guard", "Iter", "Map", "MapEntry", "Mutable", "MutableArray",
                       "MutableMap", "MutableSet", "MutableString", "Option", "Result", "Selected",
                       "Set") if n not in labels]
check("unqualified completion offers every built-in generic type", not missing, missing)
missing = [n for n in ("pub", "internal", "native", "variadic", "use", "try", "defer", "switch",
                       "with", "unit") if n not in labels]
check("unqualified completion offers the modifiers and soft keywords", not missing, missing)
# `builtin` is not among them on purpose: a program cannot declare one, so completing it would
# offer a keyword that does not type-check outside the prelude.
check("unqualified completion does not offer 'builtin'", "builtin" not in labels, "offered")
# A duplicate is invisible in an editor's own dedup but means the list was edited without being
# read, which is how the other two defects arrived.
dupes = sorted({n for n in labels if labels.count(n) > 1})
check("unqualified completion has no duplicate labels", not dupes, dupes)

r = request("workspace/symbol", {"query": "doub"})
check("workspace symbols by name", any(x["name"] == "double" and x["location"]["uri"] == util_uri for x in r["result"]), r)
r = request("textDocument/documentHighlight", {"textDocument": {"uri": main_uri}, "position": {"line": 9, "character": 8}})
# Exactly five, not "at least": `val a = 21` and its four uses. The `>= 5` this replaces passed
# just as happily on the textual version, which answered with every word that matched -- so the
# assertion sanctioned the defect it was meant to cover (docs/reviews/tasks.md F77).
check("document highlight finds every use of a name", len(r["result"]) == 5, r)

# The shape that showed the old handler was textual: a field declaration, a struct-literal field
# label, `p.x`, and a shadowing binding with its own use, all spelled `x`. Seven hits before, two
# now -- the declaration and its one real use.
shadow = ('struct Holder {\n    x: int\n}\n\nfn main() {\n    val x = 1\n    val p = Holder { x: 9 }\n'
          '    print(x + p.x)\n    if true {\n        val x = 2\n        print(x)\n    }\n}\n')
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 9}, "contentChanges": [{"text": shadow}]})
diagnostics_for(main_uri)
r = request("textDocument/documentHighlight", {"textDocument": {"uri": main_uri}, "position": {"line": 5, "character": 8}})
lines = sorted(h["range"]["start"]["line"] for h in r["result"])
check("document highlight resolves rather than matching text", lines == [5, 7], lines)
# And from the shadow's own side: its declaration and its use, not the outer binding's.
r = request("textDocument/documentHighlight", {"textDocument": {"uri": main_uri}, "position": {"line": 10, "character": 14}})
lines = sorted(h["range"]["start"]["line"] for h in r["result"])
check("document highlight sees the inner binding from inside the shadow", lines == [9, 10], lines)

# Semantic tokens. The point of the layer is `mutable` on a `var`'s *uses*, which no regex grammar
# can mark because it would have to resolve the name to its declaration -- and the use site is
# where a one-character val/var typo does its damage (docs/DESIGN.md's own note, F76, F78).
legend = init["result"]["capabilities"]["semanticTokensProvider"]["legend"]
types, mods = legend["tokenTypes"], legend["tokenModifiers"]
check("semantic tokens are advertised with a legend", "variable" in types and "mutable" in mods, legend)

sem = ('fn helper(count: int) -> int {\n    return count\n}\n\nfn main() {\n    val fixed = 1\n'
       '    var moving = 2\n    moving = moving + fixed\n    print(helper(moving))\n}\n')
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 11}, "contentChanges": [{"text": sem}]})
diagnostics_for(main_uri)
r = request("textDocument/semanticTokens/full", {"textDocument": {"uri": main_uri}})
data = r["result"]["data"]
check("semantic tokens come back delta-encoded in fives", data and len(data) % 5 == 0, len(data) if data else data)

# Decode to (line, col, text, type, [modifiers]) so the assertions below read as what they mean.
decoded, line_n, col_n = [], 0, 0
src_lines = sem.split("\n")
for k in range(0, len(data), 5):
    dl, dc, length, ty, mo = data[k:k + 5]
    line_n += dl
    col_n = dc if dl else col_n + dc
    decoded.append((line_n, col_n, src_lines[line_n][col_n:col_n + length], types[ty],
                    sorted(mods[i] for i in range(len(mods)) if mo >> i & 1)))

moving = [d for d in decoded if d[2] == "moving"]
check("every occurrence of a `var` is marked mutable, not just its declaration",
      len(moving) == 4 and all("mutable" in d[4] for d in moving), moving)
check("the `var`'s declaration is the only one marked as such",
      sum(1 for d in moving if "declaration" in d[4]) == 1, moving)
fixed_toks = [d for d in decoded if d[2] == "fixed"]
check("a `val` is readonly at its uses too",
      len(fixed_toks) == 2 and all("readonly" in d[4] for d in fixed_toks), fixed_toks)
check("a parameter is a parameter and is readonly",
      all(d[3] == "parameter" and "readonly" in d[4] for d in decoded if d[2] == "count"),
      [d for d in decoded if d[2] == "count"])
check("a function is a function", any(d[2] == "helper" and d[3] == "function" for d in decoded), decoded)

# `defaultLibrary` replaces a hand-maintained list of names with a fact, and `deprecated` gives the
# compiler's own warning a place in the editor.
lib = ('use net\n\nfn main() {\n    val l = unwrap(net.listen("127.0.0.1", 0))\n    print(str(net.listener_port(l)))\n}\n')
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 12}, "contentChanges": [{"text": lib}]})
diagnostics_for(main_uri)
r = request("textDocument/semanticTokens/full", {"textDocument": {"uri": main_uri}})
data = r["result"]["data"]
lib_lines = lib.split("\n")
line_n = col_n = 0
seen = {}
for k in range(0, len(data), 5):
    dl, dc, length, ty, mo = data[k:k + 5]
    line_n += dl
    col_n = dc if dl else col_n + dc
    seen[lib_lines[line_n][col_n:col_n + length]] = sorted(mods[i] for i in range(len(mods)) if mo >> i & 1)
check("a bundled-module function is marked defaultLibrary",
      "defaultLibrary" in seen.get("listen", []), seen)

# a folder holds its own module under its own name: `use shapes` is shapes/shapes.zm
os.makedirs(os.path.join(tmp, "shapes"))
shapes_path = os.path.join(tmp, "shapes", "shapes.zm")
open(shapes_path, "w").write("pub struct Point {\n    x: int\n}\n\npub fn origin() -> Point {\n    return Point { x: 0 }\n}\n")
folder_main = "use shapes\nuse shapes.Point\n\nfn main() {\n    val p = shapes.origin()\n    print(p.x)\n}\n"
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 5}, "contentChanges": [{"text": folder_main}]})
check("a folder module type-checks", diagnostics_for(main_uri) == [])
r = goto(0, 6)
check("ctrl+click on a folder module's path", r and r["uri"] == "file://" + shapes_path and r["range"]["start"]["line"] == 0, r)
r = goto(1, 14)
check("a name imported from a folder module", r and r["uri"] == "file://" + shapes_path and r["range"]["start"]["line"] == 0, r)
r = goto(4, 21)
check("a call into a folder module", r and r["uri"] == "file://" + shapes_path and r["range"]["start"]["line"] == 4, r)

# a name bound by `if Some(x) = ...` goes to its pattern
bind_main = "fn main() {\n    val o: Option<int> = Option<int>.Some(1)\n    if Some(x) = o {\n        print(x)\n    }\n}\n"
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 6}, "contentChanges": [{"text": bind_main}]})
check("an `if Some(x) = o` block type-checks", diagnostics_for(main_uri) == [])
r = goto(3, 14)
check("a name bound by `if Some(x) = o` goes to the pattern", r and r["uri"] == main_uri and r["range"]["start"]["line"] == 2, r)

# and one bound by a switch arm
arm_main = "fn main() {\n    val o: Option<int> = Option<int>.Some(1)\n    switch o {\n        Some(v) { print(v) }\n        None { print(0) }\n    }\n}\n"
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 7}, "contentChanges": [{"text": arm_main}]})
diagnostics_for(main_uri)
r = goto(3, 24)
check("a switch arm binder goes to its arm", r and r["uri"] == main_uri and r["range"]["start"]["line"] == 3, r)

messy = 'fn main(){\nval x=1\nprint(x)\n}\n'
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 3}, "contentChanges": [{"text": messy}]})
diagnostics_for(main_uri)
r = request("textDocument/formatting", {"textDocument": {"uri": main_uri}, "options": {"tabSize": 4}})
check("formatting returns an edit", r["result"] and "fn main() {" in r["result"][0]["newText"], r)

r = request("shutdown", None)
notify("exit", None)
check("exit code 0 after shutdown", proc.wait(timeout=10) == 0)
print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
