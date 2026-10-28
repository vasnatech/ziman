# `websites`

One directory per site, named after the domain it is served at. Each directory is a complete static
site -- no build step, no dependencies, no generator.

| directory | domain | what it is |
| --- | --- | --- |
| `ziman.dev/` | ziman.dev | The language's public page: the pitch, a runnable taste, real error output, the standard library, and links to the manuals and examples in this repository. |

## Deploying

`ziman.dev/` is deployed by **`.github/workflows/pages.yml`** on every push to `main` that touches
`websites/ziman.dev/**` (or the workflow itself), and on demand through *Actions -> Deploy
ziman.dev -> Run workflow*.

The workflow exists because **GitHub Pages serves the repository root or `docs/`, not a nested
directory**: it uploads `websites/ziman.dev/` alone as the Pages artifact. Before uploading it
checks the things nothing else would catch in a site with no build step -- that `index.html` and
`logo.svg` are present, that `CNAME` still says `ziman.dev`, that the title is intact, and that
`index.html` parses with no unclosed or stray tags.

`actions/configure-pages` runs with `enablement: true`, so the workflow turns Pages on by itself;
there is no setting to remember in the web interface.

**DNS is separate.** `ziman.dev` is an apex domain, so it needs four `A` records and four `AAAA`
records at the registrar pointing at GitHub's Pages addresses (185.199.108-111.153 and
2606:50c0:8000-8003::153), plus a `CNAME` on `www` to `vasnatech.github.io` if that host is wanted.
Until those exist the deployment still succeeds, but `ziman.dev` does not resolve -- and because
`CNAME` ships in the artifact, the `vasnatech.github.io/ziman` address redirects to the custom
domain rather than serving the site itself.

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
