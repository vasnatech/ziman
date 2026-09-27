# Review prompts

Two prompts for independent reviews of Ziman, meant to be re-run every so often (before a release, after a large batch of changes) to see what changed.

| Prompt | Reviewer | Reads | Writes |
|---|---|---|---|
| `language-design-review.prompt.md` | a programming-language design and implementation expert | the manuals, `src/`, `lib/`, the tests, and the backend as evidence | `../language-design-review-{DATE}.md` |
| `developer-experience-code-review.prompt.md` | a backend developer who learns Ziman from the manuals, then reviews the library and the Ziman backend | the manuals, `lib/`, `health-record-backend-ziman`, the Spring backend as the reference | `../developer-experience-code-review-{DATE}.md` |

## How to run

Start each as a fresh general-purpose agent (they need no context), pasting the text after the `---` line with `{DATE}` replaced by today's date. They can run in parallel, but the developer review starts servers and runs `tests/run-all.sh`, so do not run the backend tests yourself at the same time. Each reviewer only writes its own report; nothing else in the repos is touched.

Each prompt tells the reviewer to read the earlier reports last, and to re-run their reproductions to say what is fixed, what still holds and what it disagrees with. Keep old reports: they are the baseline for the next run.

The reports so far: `language-design-review.md` and `developer-experience-code-review.md` (2026-09-26, commit `766d26c` of ziman), and `pl-expert-review-2026-09-23.md` (an earlier one).
