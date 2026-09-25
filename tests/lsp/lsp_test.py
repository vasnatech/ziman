#!/usr/bin/env python3
"""Drives `ziman --lsp` over stdio and checks diagnostics, formatting, symbols,
definition, hover, completion and shutdown."""
import json, os, subprocess, sys, tempfile

ZIMAN = sys.argv[1] if len(sys.argv) > 1 else "./ziman"
proc = subprocess.Popen([ZIMAN, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
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
