# `websites`

One directory per site, named after the domain it is served at. Each directory is a complete static
site -- no build step, no dependencies, no generator.

| directory | domain | what it is |
| --- | --- | --- |
| `ziman.dev/` | ziman.dev | The language's public page: the pitch, a runnable taste, real error output, the standard library, and links to the manuals and examples in this repository. |

## Deploying

`ziman.dev/` is deployed to **Cloudflare Pages** by `.github/workflows/cloudflare-pages.yml`, on
every push to `main` that touches `websites/ziman.dev/**`, and on demand through *Actions -> Deploy
ziman.dev (Cloudflare) -> Run workflow*. It is a Direct Upload of the directory: no build step,
because the site is three static files.

The job first runs `python3 tools/check_site.py websites/ziman.dev` -- the same check you can run
locally, and the only thing in the repository that looks at `websites/` at all (`make check` does
not). It asserts the files are present, the title survived, `CNAME` still names the directory's
domain, and `index.html` has no unclosed or stray tags. Then `cloudflare/wrangler-action` uploads
the directory with the `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` repository secrets.

**Live at <https://ziman.dev> and <https://www.ziman.dev>.** Both are `CNAME` records to
`ziman-dev.pages.dev`, proxied, written by Cloudflare itself when the custom domains were added to
the Pages project -- the apex needs no `A`/`AAAA` workaround because Cloudflare flattens it.
Certificates are Cloudflare's and renew themselves. The project also keeps its own
`ziman-dev.pages.dev` address, and each deployment gets a permanent preview URL of its own.

`CNAME` in the site directory sets nothing -- neither host reads it -- but the check asserts it
still names the directory's domain, so it stays as a record of what the directory is for.

**A missing path returns the page with a 200, not a 404**, which is Cloudflare Pages' default for a
site with no `404.html`. Harmless for one page; add a `404.html` to the directory if that ever
matters.

### Known for next time

`wrangler pages project create` now delegates to "the latest version of Cloudflare Pages, now part
of Cloudflare Workers", and that delegation fails unless it is run where it can find static files.
This project was created with `--force`, which uses Cloudflare Pages directly; wrangler says that is
needed once only. Cloudflare is folding Pages into Workers, so moving this to a Worker with static
assets is a likely future change -- cheap for three files, but it would mean a different workflow
(`wrangler deploy` with an `assets` block rather than `wrangler pages deploy`).

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
