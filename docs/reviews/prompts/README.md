# Review prompts

Three prompts for independent reviews of Ziman, meant to be re-run every so often (before a release, after a large batch of changes) to see what changed. Each takes a different reviewer's seat, and the three deliberately do not overlap: one judges the language, one judges using it, one judges the C it is written in.

| Prompt | Reviewer | Reads | Writes |
|---|---|---|---|
| `language-design-review.prompt.md` | a programming-language design and implementation expert | the manuals, `src/`, `lib/`, the tests, and the backend as evidence | `../language-design-review-{DATE}.md` |
| `developer-experience-code-review.prompt.md` | a backend developer who learns Ziman from the manuals, then reviews the library and the Ziman backend | the manuals, `lib/`, `health-record-backend-ziman`, the Spring backend as the reference | `../developer-experience-code-review-{DATE}.md` |
| `c-implementation-review.prompt.md` | a senior C programmer, judging the implementation **as C** rather than the language it implements | `src/` (not the generated `*_source.h`), `docs/MEMORY.md`, `tests/unit/`, the Makefile and the vendored collector's flags | `../c-implementation-review-{DATE}.md` |

## How to run

Start each as a fresh general-purpose agent (they need no context), pasting the text after the `---` line with `{DATE}` replaced by today's date. They can run in parallel, but two of them contend for the machine: the developer review starts servers and runs `tests/run-all.sh`, and the C review builds the whole project several times over with sanitizers and fuzzes the parser — so do not run the backend tests yourself alongside either, and expect the C review to be the slow one. Each reviewer only writes its own report; nothing else in the repos is touched, and the C prompt says explicitly that `git status` must show only its report when it finishes, since it is the one with a reason to try a change.

Each prompt tells the reviewer to read the earlier reports last, and to re-run their reproductions to say what is fixed, what still holds and what it disagrees with. Keep old reports: they are the baseline for the next run.

The reports so far: `language-design-review.md` and `developer-experience-code-review.md` (2026-09-26, commit `766d26c` of ziman), and `pl-expert-review-2026-09-23.md` (an earlier one).
