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

util_src = 'pub struct Point {\n    x: int\n    y: int\n}\n\npub fn double(n: int) -> int {\n    return n * 2\n}\n'
main_bad = 'use util\n\nfn main() {\n    val a: int = "no"\n    print(util.double(a))\n    print(missing)\n}\n'
notify("textDocument/didOpen", {"textDocument": {"uri": util_uri, "languageId": "ziman", "version": 1, "text": util_src}})
diagnostics_for(util_uri)
notify("textDocument/didOpen", {"textDocument": {"uri": main_uri, "languageId": "ziman", "version": 1, "text": main_bad}})
d = diagnostics_for(main_uri)
check("two errors reported at once", len(d) == 2, d)
check("first error on line 4", d and d[0]["range"]["start"]["line"] == 3, d)

main_ok = 'use util\n\nfn main() {\n    val a = 21\n    print(util.double(a))\n}\n'
notify("textDocument/didChange", {"textDocument": {"uri": main_uri, "version": 2}, "contentChanges": [{"text": main_ok}]})
check("fixing the buffer clears diagnostics", diagnostics_for(main_uri) == [])

r = request("textDocument/definition", {"textDocument": {"uri": main_uri}, "position": {"line": 4, "character": 16}})
check("go to definition crosses files", r["result"] and r["result"]["uri"] == util_uri and r["result"]["range"]["start"]["line"] == 5, r)

r = request("textDocument/hover", {"textDocument": {"uri": main_uri}, "position": {"line": 4, "character": 16}})
check("hover shows the signature", r["result"] and "fn double(n: int) -> int" in r["result"]["contents"]["value"], r)

r = request("textDocument/documentSymbol", {"textDocument": {"uri": util_uri}})
names = sorted(s["name"] for s in r["result"])
check("document symbols", names == ["Point", "double"], names)

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

r = request("workspace/symbol", {"query": "doub"})
check("workspace symbols by name", any(x["name"] == "double" and x["location"]["uri"] == util_uri for x in r["result"]), r)
r = request("textDocument/documentHighlight", {"textDocument": {"uri": main_uri}, "position": {"line": 9, "character": 8}})
check("document highlight finds every use of a name", len(r["result"]) >= 5, r)

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
