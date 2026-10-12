---
name: ziman-style-architect
description: Learn Ziman from this repository, then produce a formatting style reference with worked examples and a syntax-colouring strategy. Load when asked to design, document, audit or revise Ziman's code formatting, its style guide, its syntax highlighting, editor themes, TextMate scopes or LSP semantic tokens. Treats `ziman --fmt` as the oracle and the existing VS Code grammar as prior art, never inventing a style from scratch.
---

# Ziman style architect

**The prompt lives in the repository, not here.** Read
`docs/reviews/prompts/code-style-and-colouring.prompt.md` and follow everything after its `---`
line, replacing `{DATE}` with today's date.

This file is deliberately a pointer rather than a copy. The same instructions existing in two places
is how a project ends up with two versions of them, and this one has paid for that lesson more than
once -- see `docs/reviews/prompts/README.md`, which lists the four review prompts and what each is
for.

Two things the prompt will tell you, repeated here only because they decide whether the work is
worth anything:

- **`ziman --fmt` is the oracle.** Ziman already has a formatter (`src/format.c`), and `make
  fmt-check` enforces it over the whole library as part of `make check`. Every formatting rule must
  be verified by writing a file, running the formatter on it, and stating what it did.
- **A formatter decision you disagree with is a finding, not a rule.** It goes in the report as a
  question for the maintainer. You do not write it up as the style, and you do not change the
  formatter to match your taste.

The difference between invoking this skill and running the prompt as a fresh agent: the skill reuses
whatever context the current session already has, which is cheaper but less independent. For a
report meant to stand on its own, start a fresh general-purpose agent with the prompt instead.
