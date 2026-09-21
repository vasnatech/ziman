# Ziman for VS Code

Syntax highlighting for `.zm` files. A TextMate grammar only — no
language server, no diagnostics, no autocomplete. It highlights:

- keywords (`fn`, `val`, `var`, `if`, `else`, `while`, `return`, `struct`,
  `with`, `pub`, `use`, `as`, `native`)
- the four primitive types (`int`, `float`, `bool`, `string`)
- comments, strings (with `\n`/`\t`/`\"`/`\\` escapes), numbers, `true`/`false`
- function names at both declaration (`fn greet`) and call (`greet(...)`)
  sites
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
ln -s "$(pwd)" ~/.vscode/extensions/ziman-lang.ziman-0.1.0
# WSL/remote: ~/.vscode-server/extensions/ instead of ~/.vscode/extensions/
```

Then reload the window (`Cmd/Ctrl+Shift+P` → "Developer: Reload Window").
`.zm` files should be recognized automatically; if not, click the
language indicator in the status bar and pick "Ziman".

## Files

- `package.json` — the extension manifest (declares the `ziman` language
  and points at the grammar).
- `language-configuration.json` — comment toggling (`//`) and bracket/
  quote auto-closing.
- `syntaxes/ziman.tmLanguage.json` — the actual TextMate grammar.
- `showcase.zm` (+ `models/user.zm`, which it `use`s) — a file exercising
  every highlighted construct, for eyeballing changes to the grammar.
  It's genuine, runnable Ziman, not just plausible-looking text: verified
  with `ziman editors/vscode/showcase.zm` from the repo root.
