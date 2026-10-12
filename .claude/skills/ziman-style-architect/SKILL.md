---
name: ziman-style-architect
description: Learn Ziman from this repository, then produce two reports - a formatting style reference with worked examples, and a syntax-colouring strategy. Load when asked to design, document, audit or revise Ziman's code formatting, its style guide, its syntax highlighting, editor themes, TextMate scopes or LSP semantic tokens. Treats `ziman --fmt` as the oracle and the existing VS Code grammar as prior art, never inventing a style from scratch.
---

# Ziman style architect

You are acting as an architect for **Ziman**, a programming language implemented in hand-written C11 in this repository. Two deliverables: a **formatting reference** and a **colouring strategy**. Both are reports, written for a human reader, full of worked examples.

Read this whole file before starting.

## The one rule that makes this task different

**Ziman already has a formatter and already has a syntax grammar.** You are not designing a style for a blank language. Two artifacts are authoritative and you do not get to overrule them from taste:

| Artifact | What it is | Status |
|---|---|---|
| `src/format.c` (~740 lines) | The formatter behind `ziman --fmt` | **The oracle.** What it prints *is* the format |
| `editors/vscode/syntaxes/ziman.tmLanguage.json` | TextMate grammar, ~32 named scopes | **Prior art.** Document and justify it before changing it |

`make fmt-check` runs `ziman --fmt --check` over every `.zm` file in `lib`, `docs/lib/examples`, `tests/pg`, `tests/mysql`, `tests/tls`, `tests/shutdown` and `tests/bench`, and it is part of `make check`. So the format is not advisory — it is enforced on every commit.

**Therefore: every formatting rule you write must be one you have verified by running the formatter.** Never state a rule from intuition about what a language "should" look like. The procedure for each rule is:

1. Write a small `.zm` file that exercises the construct, in a scratch directory.
2. Run `./ziman --fmt thatfile.zm` and read what comes back.
3. State the rule as what the formatter *did*, with the before/after as the example.

If you believe a formatter decision is wrong, that is a **finding**, reported in its own section as a question for the maintainer — not a rule you write as though it were the style. Say which construct, what it does, what you expected, and why. Do not "fix" it.

## How to learn Ziman, in order

Do not skim these; the language has decisions that will surprise you, and several of them bear directly on formatting.

1. **`docs/SYNTAX.md`** — the normative language reference. This is the single most important file. It is long; read it.
2. **`docs/TUTORIAL.md`** — the language as a newcomer meets it.
3. **`docs/DESIGN.md`**, specifically these entries (find them via the index at the top):
   - "The formatter (`ziman --fmt`, R17)" — the original design
   - "Formatter: wrapping, comment alignment, sorted `use`, stdin" (F11)
   - "`--fmt --canonical`: settling brace placement, which plain `--fmt` will not" (F12)
   These three tell you *why* the formatter does what it does, including what it deliberately refuses to do. **A rule the formatter deliberately leaves alone is itself a rule**, and the reason belongs in your report.
4. **`lib/`** — about 40 modules of real, formatted Ziman. `lib/http/server.zm` is the largest and shows the house style at scale. Everything here passes `fmt-check`, so it is all evidence.
5. **`editors/vscode/`** — `syntaxes/ziman.tmLanguage.json`, `language-configuration.json`, `snippets/`, and `showcase.zm`, which exists to show the highlighting off.
6. **`src/format.c`** — read it last, to confirm what you observed, not to derive rules from. Reading code and reporting intent is how you end up documenting a bug as a feature.

Write a few programs of your own and run them (`./ziman file.zm`). Facts about this language that formatting advice routinely gets wrong, and which you should confirm rather than assume: struct fields are newline-separated; `with` is a prefix form (`with base { field: value }`); a call cannot mix positional and named arguments; an optional parameter can be omitted only in a named call; a line inside a call argument continues only when it *ends* with an operator; `use a/b` uses a slash and `use a.b` imports a symbol; `switch` is exhaustive with no default arm; a variadic parameter takes an array directly.

## Deliverable 1 — the formatting reference

A report a programmer can read front to back and then format Ziman by hand the way `--fmt` would. Cover, each with at least one worked example taken from a real run of the formatter:

- **Indentation**: width, what increases it, what does not.
- **Line length and wrapping**: the default width, where a long signature breaks, where a long call breaks, where a long type breaks, where a chain breaks. `--fmt-width` exists; say what it changes.
- **Declarations**: functions (including generic ones, variadics, optional parameters), `struct`, `enum`, module-level `val`, `native fn`.
- **Statements and expressions**: `if`/`else`, `switch` and its arms, `for`, `while`, `defer`, `spawn`, lambdas, `try`, `match`-like binding forms (`if Some(x) = ...`).
- **Literals and collections**: arrays, maps, sets, struct literals, `with` overrides, long literal lists.
- **`use` lines**: ordering (the formatter sorts them — show how), grouping, aliases.
- **Attributes**: `#[...]` on a declaration and `#![...]` on a file, and their position relative to a doc comment. There is a real hazard here recorded in the project's history: an attribute between a doc comment and its declaration once silently cost declarations their documentation in the generated manual. Check how the formatter places them and say so.
- **Comments**: line comments, their alignment (the formatter aligns trailing comments — show it), comment blocks above declarations, and what the formatter will *not* reflow.
- **Blank lines**: where they are preserved, collapsed, or inserted.
- **Operators and spacing**: binary operators, unary, `:` in annotations, `->`, generics `<>`, ranges `a..b`.
- **What `--fmt --canonical` changes that plain `--fmt` does not** — and why that split exists at all.
- **The limits**: the formatter refuses input nested deeper than 200 brackets and bounds its output at 16 MB. Both have arithmetic reasons in DESIGN.md. State them; a style guide that pretends a tool has no limits is a style guide someone will hit.

End with a short **findings** section: anything the formatter does that you think is wrong or under-specified, phrased as a question.

## Deliverable 2 — the colouring strategy

The point is not a palette. The point is **what the colours mean** and **which distinctions are worth making**. Work in three layers and keep them separate, because they have different capabilities:

1. **What the TextMate grammar already does.** Enumerate the ~32 scopes in `ziman.tmLanguage.json`, say what each matches, and group them into a small number of *semantic* categories a theme author can reason about. Justify each mapping. Note any construct in `SYNTAX.md` that has **no scope at all** — that is a gap, and finding it is the work.
2. **What a regex grammar structurally cannot do.** This is the most valuable part of the report, so be concrete. A TextMate grammar cannot tell a type parameter from a struct name, a `var` from a `val` at the use site, a deprecated symbol from a live one, an imported name from a local one, or a shadowed binding from the thing it shadows. For each distinction, say whether it is worth colouring and why.
3. **The semantic-token layer.** The LSP (`src/lsp.c`, `ziman --lsp`) today advertises `documentHighlightProvider` and **no semantic tokens** — verify that before relying on it. Propose the token types and modifiers Ziman should emit, mapped to the LSP's standard set where one fits and named explicitly where none does. Say what each buys that layer 1 cannot.

Then give the strategy proper:

- A recommended **category-to-role mapping** (what should be visually loudest, what should recede) with the reasoning. Ziman's design values explicitness — mutability, visibility and effects are written, not inferred — so argue for or against making those written markers visually prominent.
- **Light and dark** guidance, including contrast constraints, and colour-blind safety: never let a distinction rest on red-versus-green alone.
- A **worked sample**: take `editors/vscode/showcase.zm` (or a file you write that exercises every category) and present it as a table of token → category → role, so the mapping can be checked line by line.
- **How to verify a theme is right**, as steps someone can actually run.

## Output

Write both reports as files, and say where you put them:

- `docs/FORMATTING.md` — deliverable 1
- `docs/COLOURING.md` — deliverable 2

Match the house style of `docs/SYNTAX.md` and `docs/DESIGN.md`: prose that states the rule and then the reason, fenced `ziman` examples, no bullet-point padding. These documents are not generated, so they are yours to write by hand — but check whether `make docs-check` has anything to say about new files under `docs/`, and do not break it.

Do not commit anything. Leave the files in the working tree and report what you wrote, what you verified by running, and what you are unsure about.

## Honesty requirements

This project's history is a long record of roadmap entries that had quietly drifted from the code. Assume this task's premises have too.

- **Verify before asserting.** Every rule traced to a formatter run, every scope traced to the grammar file, every LSP claim traced to `src/lsp.c`.
- **Say what you did not check.** A report that names its gaps is worth more than one that implies full coverage.
- **Do not invent examples.** If an example is in your report, you ran it. Hand-transcribing a snippet and getting it subtly wrong has cost this project real time.
- **Distinguish three things explicitly**: what the formatter does, what the style guide recommends where the formatter is silent, and what you think should change.
