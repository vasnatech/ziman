# Prompt: code style and colouring architecture for Ziman

Run this as a fresh general-purpose agent (it needs no prior context). Replace `{DATE}` with today's date (`YYYY-MM-DD`) before sending. The agent only writes its report.

Unlike the other three prompts here, this one is not a critical review of work already done — it is an architecture task with two deliverables in one report: a formatting reference a programmer could follow by hand, and a syntax-colouring strategy. It is still a *review* in the sense that matters: both deliverables must describe what the tools in the repository actually do, verified by running them, not what a language of this shape ought to do.

---

You are a senior language-tooling architect: someone who has designed a formatter and a syntax-highlighting scheme for a real language, and has had to defend both to the people who use them every day (think: the people behind `gofmt`, `rustfmt`, Prettier, or a tree-sitter grammar plus an editor theme). Your task is to LEARN a small language called Ziman from its own repository, and then write ONE report containing two things: **how Ziman code should be formatted**, with worked examples, and **a strategy for colouring it**. This is a read-only investigation: do NOT modify any file except the one report named below, and do not run `git commit` or `git push`.

## The constraint that makes this task different from a blank-page design

**Ziman already has a formatter, and it is enforced.** Before you write a single rule, understand this:

- `./ziman --fmt file.zm` prints the file formatted. This is **the oracle**. What it prints *is* Ziman's format.
- `src/format.c` (about 740 lines) is the implementation.
- `make fmt-check` runs `./ziman --fmt --check` over every `.zm` file under `lib`, `docs/lib/examples`, `tests/integration/pg`, `tests/integration/mysql`, `tests/integration/tls`, `tests/integration/shutdown` and `tests/bench` — and it is part of `make check`, which gates every commit. So the format is not advice; it is enforced on the whole library.

**Ziman also already has a syntax grammar**: `editors/vscode/syntaxes/ziman.tmLanguage.json`, a TextMate grammar naming roughly 32 scopes, alongside `editors/vscode/language-configuration.json`, `editors/vscode/snippets/` and `editors/vscode/showcase.zm`. That is prior art. Document and justify it before proposing a change to it.

So: **every formatting rule in your report must be one you verified by running the formatter.** For each rule, the procedure is (1) write a small `.zm` file exercising the construct, (2) run `./ziman --fmt` on it, (3) state the rule as what the formatter *did*, using that run as the example. Never state a rule from intuition about what the language "should" look like.

If you think a formatter decision is wrong, that is a **finding** — a question for the maintainer in its own section, with the construct, the behaviour, what you expected and why. It is not a rule you write up as though it were the style, and you do not change the formatter to match your taste.

## Where things are

- The Ziman repo: `/home/mfg/workspace/vasna.tech/repos/ziman`. Build with `make` (the binary is `./ziman`); `make -j8 ziman` is enough and takes under a minute.
- **Start by recording the commit (`git rev-parse --short HEAD`) and the result of `make fmt-check` and `make test`.** Put both at the top of your report. If `fmt-check` is not clean on a fresh checkout, stop and say so — it means the oracle disagrees with the committed tree and nothing else you conclude is trustworthy.
- Language reference: `docs/syntax/` (very long; read it thoroughly — it is the normative description and the single most important file for this task). `docs/TUTORIAL.md` is the language as a newcomer meets it.
- `docs/design/journal/` is a dated decision log. Find these entries through the index at the top and read them in full, because they say what the formatter deliberately refuses to do and why:
  - "The formatter (`ziman --fmt`, R17)"
  - "Formatter: wrapping, comment alignment, sorted `use`, stdin" (F11)
  - "`--fmt --canonical`: settling brace placement, which plain `--fmt` will not" (F12)
  **A rule the formatter deliberately leaves alone is itself a rule**, and its reason belongs in your report.
- About 40 modules of real, formatted Ziman: `lib/prelude.zm`, `lib/core/*.zm`, `lib/db/*.zm`, `lib/http/*.zm`, `lib/time/*.zm`. All of it passes `fmt-check`, so all of it is evidence. `lib/http/server.zm` is the largest file and shows the house style at scale.
- A real application, as evidence of the style under pressure: `/home/mfg/workspace/vasna.tech/repos/health-record/health-record-backend-ziman` (about 13,000 lines of Ziman; its README explains it).
- The editor integration: `editors/vscode/` as listed above. `src/lsp.c` is the language server (`./ziman --lsp`).
- Scratch files go **only** under `/tmp/ziman_style/`, never inside either repository. `git status` in both repos must show nothing but your report when you finish.
- Useful commands: `./ziman file.zm` runs, `./ziman --check file.zm` type-checks, `./ziman --fmt file.zm` formats, `./ziman --fmt --canonical file.zm` formats more aggressively, `./ziman --fmt --width=N file.zm` changes the wrap width (default 120, `0` never wraps; check `./ziman --help` for the exact spelling rather than trusting this line), `./ziman --lint file.zm` reports style findings. `./ziman --help` is the full list.
- Earlier reviews are in `docs/reviews/*.md`. Read them AFTER forming your own view, then say which of their formatting or tooling findings are now fixed, which still hold, and which you disagree with — checking by re-running their reproductions rather than trusting the text.

## Learning the language first

Do not skip this and do not skim. Read `docs/syntax/`, then write and run several programs of your own. Ziman has decisions that will surprise you and several bear directly on how code is laid out. Confirm each of these by experiment rather than assuming the shape you are used to:

- struct fields are newline-separated, not comma-separated
- `with` is a **prefix** form: `with base { field: value }`
- a call cannot mix positional and named arguments
- an optional parameter can be omitted **only** in a named call
- a line inside a call argument continues only when it **ends** with an operator
- `use a/b` is a path (slash-separated); `use a.b` imports the symbol `b` from module `a`
- `switch` is exhaustive with no default arm, and is usable as a value in many positions
- a variadic parameter accepts an array directly
- an empty array literal needs a type from context
- there are no methods — free functions only — so there is no method-chain layout question, and you should say so rather than inventing one

## Part 1 — the formatting reference

Write it so that a programmer could read it front to back and then format Ziman by hand the way `--fmt` would. Every item below needs at least one worked example taken from a real run, shown as input and output where they differ:

1. **Indentation**: width, what increases it, what does not.
2. **Line length and wrapping**: the default target width; where a long function signature breaks; a long call; a long type; a long condition. Say what `--width=N` changes and what it does not. Establish early whether the formatter ever *moves* a line break the author wrote, or only decides where to add one: the answer governs most of this part.
3. **Declarations**: functions (generic, variadic, with optional parameters), `struct`, `enum`, module-level `val`, `native fn`, visibility markers.
4. **Statements and expressions**: `if`/`else`, `switch` and its arms, `for`, `while`, `defer`, `spawn`, lambdas, `try`, and the binding forms (`if Some(x) = ...`).
5. **Literals and collections**: arrays, maps, sets, struct literals, `with` overrides, and long literal lists.
6. **`use` lines**: the formatter sorts them — show the ordering it produces, including how module imports and symbol imports interleave, and what happens to aliases.
7. **Attributes**: `#[...]` on a declaration and `#![...]` on a file, and their placement relative to a doc comment. There is a real hazard in this area recorded in the project's history — an attribute written between a doc comment and its declaration once silently cost several declarations their documentation in the generated manual. Establish by experiment where the formatter puts them and what it preserves.
8. **Comments**: line comments; the alignment of trailing comments (the formatter aligns them — show it); comment blocks above declarations; and what the formatter will **not** reflow.
9. **Blank lines**: where they are preserved, collapsed, or inserted.
10. **Operators and spacing**: binary and unary operators, `:` in annotations, `->`, generic `<>`, ranges `a..b`.
11. **What `--fmt --canonical` changes that plain `--fmt` does not**, and why that split exists at all.
12. **The limits**: the formatter refuses input nested deeper than 200 brackets and bounds its output at 16 MB. Both have arithmetic reasons given in `docs/design/journal/`. State them — a style guide that pretends its tool has no limits is one somebody will walk into.

## Part 2 — the colouring strategy

The deliverable is not a palette. It is **what the colours mean** and **which distinctions are worth making**. Keep three layers separate, because they have different capabilities:

1. **What the TextMate grammar already does.** Enumerate the scopes in `ziman.tmLanguage.json`, say what each matches, and group them into a small number of semantic categories a theme author can reason about. Justify each mapping. Then find the gaps: name any construct in `docs/syntax/` that has **no scope at all**. Finding those is the work, not a formality.
2. **What a regex grammar structurally cannot do.** Be concrete and verify each claim against the grammar file. A TextMate grammar cannot tell a type parameter from a struct name, a `var` from a `val` at its use site, a deprecated symbol from a live one, an imported name from a locally declared one, or a shadowed binding from what it shadows. For each distinction: is it worth colouring, and why?
3. **The semantic-token layer.** Check what `src/lsp.c` advertises in its initialize response before relying on it — at the time this prompt was written it offered `documentHighlightProvider` and **no semantic tokens**, which you should confirm or correct. Do not read `documentHighlightProvider` as evidence that occurrence resolution already exists: establish by experiment whether it resolves a binding or merely matches the word, because the answer decides whether a token layer is new work or a re-export of something already computed. Propose the token types and modifiers Ziman should emit, mapped onto the LSP's standard set where one fits and named explicitly where none does, and say what each buys that layer 1 cannot.

Then give the strategy itself:

- A **category-to-role mapping**: what should be visually loudest, what should recede, with the reasoning. Ziman's design values explicitness — mutability, visibility and thread-affinity are written in the source rather than inferred — so argue for or against making those written markers visually prominent. This is a judgement call and you should make it, not list options.
- **Light and dark** guidance, with contrast constraints, and colour-blind safety: no distinction may rest on red-versus-green alone.
- A **worked sample**: take `editors/vscode/showcase.zm`, or write a file that exercises every category, and present it as a table of token → category → role so the mapping can be checked line by line.
- **How to verify a theme is right**, as steps someone can actually run.

## Deliverable

Write ONE Markdown report to `/home/mfg/workspace/vasna.tech/repos/ziman/docs/reviews/code-style-and-colouring-{DATE}.md` (do not overwrite an earlier one). Structure: the commit and the `fmt-check`/`make test` results you started from; an executive summary (10 lines max); Part 1, the formatting reference; Part 2, the colouring strategy; a findings section for anything the formatter or the grammar does that you believe is wrong or under-specified, each phrased as a question for the maintainer; and an appendix of the experiments you ran (input, output, why it matters).

Tag every statement **[V]** (verified: you ran it, or read the cited `file:line`) or **[O]** (opinion). Three things must stay distinguishable throughout: what the formatter **does**, what you **recommend** where the formatter is silent, and what you think should **change**. A reader who cannot tell those apart cannot use the report.

Do not invent examples. If an example appears in your report, you ran it — hand-transcribing a snippet and getting it subtly wrong has cost this project real time before. Say plainly what you did not check; a report that names its gaps is worth more than one implying coverage it does not have.

Aim for a thorough report (roughly 4,000-7,000 words plus the appendix). When finished, reply with a 10-line summary and the report path.
