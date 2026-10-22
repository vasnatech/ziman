# Ziman for VS Code

Support for `.zm` files: a TextMate grammar for highlighting, and a client for the
built-in language server (`ziman --lsp`, see "Language server" below). The grammar highlights:

- keywords (`fn`, `val`, `var`, `if`, `else`, `while`, `for`, `in`, `break`, `continue`, `return`,
  `switch`, `defer`, `try`, `spawn`, `struct`, `enum`, `with`, `use`, `as`) and the modifiers
  (`pub`, `internal`, `native`, `builtin`, `variadic`)
- the primitive types (`int`, `float`, `bool`, `string`, `bigint`, `decimal`, `byte`, `unit`) and
  the built-in generic types, the `Mutable*` builder family among them
- comments, text blocks (`"""`), strings (with `\n`/`\t`/`\"`/`\\` escapes), numbers including
  the `123n` bigint, `19.99d` decimal and `200b` byte literals, `true`/`false`
- attributes, declaration-level (`#[derive(...)]`, `#[test]`) and file-level (`#![experimental]`)
- `::`, the field-accessor shorthand, as one operator with the field after it as a property:
  `Point::x` is `fn(p: Point) { p.x }`
- function names at both declaration (`fn greet`) and call (`greet(...)`) sites, with the
  builtins (`len`, `set`, `insert`, `get`, `union`, `append`, ...) in their own scope

**The two name lists are generated, not hand-kept.** `tools/gen_vscode_grammar.py` rewrites the
`builtin-types` and `builtin-functions` patterns from `ziman --api builtins`, and
`make vscode-grammar-check` fails when the committed grammar is not what it produces. Everything
else in the grammar is hand-written. The lists were hand-kept until 2026-10-19, by which point they
highlighted six names that had been deleted and knew none of the 28 that had been renamed.
- type/struct names, using Ziman's own enforced casing rule (a value or
  function name can never start uppercase, so any capitalized identifier
  is reliably a type) — this also lights up qualified references like
  `user.User { ... }` correctly, since `User` alone is matched regardless
  of what precedes it
- operators

Two things it deliberately does *not* try to highlight distinctly,
because a regex-only grammar can't tell them apart reliably: a module
qualifier (`user` in `user.greet(...)`) and a struct-valued local
variable (`p` in `p.x`) look identical from pure syntax — both are a
lowercase identifier immediately before a `.`. Rather than guess and be
wrong on the far more common case (plain field access), both are left in
the default text color.

## Installing it locally

This isn't published to the Marketplace. Two ways to use it:

**Quick look (no install):** open this folder in VS Code and press `F5`
— it launches an Extension Development Host window with the grammar
active. Open any `.zm` file there.

**Install it for real, in your everyday VS Code:**

```sh
# macOS/Linux
ln -s "$(pwd)" ~/.vscode/extensions/ziman-lang.ziman-0.3.0
# WSL/remote: ~/.vscode-server/extensions/ instead of ~/.vscode/extensions/
```

Then reload the window (`Cmd/Ctrl+Shift+P` → "Developer: Reload Window").
`.zm` files should be recognized automatically; if not, click the
language indicator in the status bar and pick "Ziman".

## Files

- `package.json` — the extension manifest (language, grammar, snippets, commands, keybindings, editor defaults).
- `language-configuration.json` — comment toggling (`//`), bracket/quote auto-closing (including `"""`), indentation and `// region` folding.
- `snippets/ziman.json` — the snippets above.
- `extension.js` — the language client and the commands.
- `syntaxes/ziman.tmLanguage.json` — the actual TextMate grammar.
- `showcase.zm` (+ `models/user.zm`, which it `use`s) — a file exercising
  every highlighted construct, for eyeballing changes to the grammar.
  It's genuine, runnable Ziman, not just plausible-looking text, and
  **`make showcase-check` is what keeps that true**: it type-checks the
  file and holds it to `--fmt`, inside `docs-check` and so inside
  `check-fast`. Nothing compiled it until 2026-10-21, and by then it
  called `set_union` and `map_get` — renamed to `union` and `get` by
  F195-F201 — so the demonstration of the language named two functions
  the language does not have. Add a construct here whenever the grammar
  learns one; a rule with nothing to colour in this file is a rule
  nobody will notice breaking.

## Language server

`ziman --lsp` speaks the Language Server Protocol on stdin/stdout: diagnostics
(every error the compiler finds, from the open, unsaved buffers), formatting
(`ziman --fmt`, width 120), document symbols, workspace symbols, go to definition,
hover (signatures), document highlight, and completion (keywords, builtins, the file's
declarations, `module.` members, enum variants).

**Go to definition (Ctrl+Click, F12; Ctrl+Alt+Click opens to the side)** follows:

- a `use` line: the path opens the module's file, and a name or alias goes to that
  declaration (`use models/user.User as Person`);
- a call or type through a module qualifier, including an `as` alias:
  `medications.handle_list` finds `handle_list` in `medication/controller.zm`;
- the qualifier itself (`medications` in `medications.handle_list`): the module's file;
- a name brought in bare with `use mod.Name [as alias]`;
- the **bundled library** (`mime.parse`, `json.json_get_path`, `datasource.query`, ...): the
  embedded source is written once to `~/.cache/ziman/core/<module>.zm` (read-only) and opened
  there, with hover and completion working on it too;
- an enum variant (its enum), and local variables, parameters, `for` binders, `switch` arm
  binders and lambda parameters (found by reading the function above the cursor).

Other conveniences: **Ctrl+T** searches every declaration by name (the program's own first, then
the library's); selecting a name highlights its other uses in the file; the outline shows each
file's declarations.

**Editor defaults for `.zm`** (a `configurationDefaults` block, override in your settings): format on
save, 4-space indentation, a ruler at 120, and `"""` closing like other quotes.

**Snippets** (type the prefix, Tab): `main`, `fn`, `pubfn`, `fnr` (returns a `Result`), `struct`,
`enum`, `derive`, `switch`, `switchr` (switch on a `Result`), `if`, `ifelse`, `for`, `fori`, `forr`,
`while`, `val`, `valt` (`val x = try call()`), `use`, `usea` (`as`), `usen` (`use mod.Name`),
`spawn`, `defer`, `block` (a `"""` text block), `pr`.

**Commands** (Command Palette, category "Ziman"): *Run File* (Ctrl+Alt+R, also the play button in the
editor title) runs the current file in a terminal; *Run Program (main.zm)* (Ctrl+Alt+M) runs the
`main.zm` next to it; *Check File* type-checks without running; *Restart Language Server*. Changing
`ziman.serverPath` offers to restart the server.

- **VS Code:** `cd editors/vscode && npm install`, then load the folder as an
  extension (see "Installing it locally"). Set `ziman.serverPath` if `ziman` is not on
  `PATH`. The server is covered by `make test-lsp`; the client (`extension.js`) is a thin
  wrapper that has not been exercised in a real VS Code here.
- **Neovim:** `vim.lsp.start({ name = "ziman", cmd = { "ziman", "--lsp" }, root_dir = vim.fs.root(0, "main.zm") })` in a `FileType ziman` autocmd.
- **Helix** (`languages.toml`): `[language-server.ziman]` `command = "ziman"`, `args = ["--lsp"]`, and a `[[language]] name = "ziman"` with `language-servers = ["ziman"]`.
- **Emacs (eglot):** `(add-to-list 'eglot-server-programs '(ziman-mode "ziman" "--lsp"))`.

A file is analysed as part of the program rooted at the nearest `main.zm` in its
directory or up to six parents, else on its own. Documents use full-text sync.
