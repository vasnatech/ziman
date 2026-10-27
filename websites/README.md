# `websites`

One directory per site, named after the domain it is served at. Each directory is a complete static
site -- no build step, no dependencies, no generator.

| directory | domain | what it is |
| --- | --- | --- |
| `ziman.dev/` | ziman.dev | The language's public page: the pitch, a runnable taste, real error output, the standard library, and links to the manuals and examples in this repository. |

## Deploying

**Nothing deploys these automatically yet.** The `CNAME` file in each directory names its domain, so
a site is ready for GitHub Pages or for any static host pointed at the directory:

- **GitHub Pages** serves the repository root or `docs/`, not a nested directory, so publishing
  `ziman.dev/` needs a workflow that uploads just that directory as the Pages artifact.
- **Any other static host** (a bucket, a CDN, a plain web server) can be pointed straight at the
  directory.

Either way the DNS for `ziman.dev` has to point at the host, and the `CNAME` file has to match the
domain configured there.

## Keeping the page true

The page makes claims that the repository can disprove. When one of these changes, change the page:

- The **taste example** and its output are `tests/lang/readme_example.zm` and its `.out`, run by
  `make check` on every change. The page's copy must stay character-identical to them.
- The **error messages** were captured from the built `ziman` binary with `--check`. Re-capture them
  if a diagnostic's wording changes.
- The **counts** -- fixtures in `tests/lang`, repositories in the sample backend -- and the
  **experimental markers** (`*`), which are the modules carrying `#![experimental]` in `lib/`.
- The **manual links** point at `docs/*.md` on the `main` branch; the **example links** at
  `docs/lib/examples/`.
