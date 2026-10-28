# `websites`

One directory per site, named after the domain it is served at. Each directory is a complete static
site -- no build step, no dependencies, no generator.

| directory | domain | what it is |
| --- | --- | --- |
| `ziman.dev/` | ziman.dev | The language's public page: the pitch, a runnable taste, real error output, the standard library, and links to the manuals and examples in this repository. |

## Deploying

**Cloudflare Pages is where this is going**, because Cloudflare already holds the DNS for
`vasna.tech` and will hold `ziman.dev`: on the same account the custom domain needs no apex
workaround (Cloudflare flattens it), the certificate is Cloudflare's, and the Pages project creates
the DNS record itself.

| workflow | host | state |
| --- | --- | --- |
| `.github/workflows/cloudflare-pages.yml` | Cloudflare Pages (Direct Upload) | the target; deploys as soon as the account's secrets exist |
| `.github/workflows/pages.yml` | GitHub Pages | serving <https://vasnatech.github.io/ziman/> today; **to be removed once Cloudflare serves `ziman.dev`** |

Both run on a push to `main` touching `websites/ziman.dev/**`, and both begin by running
`python3 tools/check_site.py websites/ziman.dev` -- the same check you can run locally. It asserts
the files are present, the title survived, `CNAME` still names the directory's domain, and
`index.html` has no unclosed or stray tags. Nothing else in the repository looks at `websites/`;
`make check` does not.

The Cloudflare workflow **skips its deploy step while the credentials are absent** rather than
failing, so it is committed before the account is set up without turning every push red. It says so
in the run's annotations.

### What is still needed, in order

1. **Move the zone.** `ziman.dev` is on Squarespace's nameservers (`nsb1-4.squarespacedns.com`) and
   resolves to Squarespace parking addresses. Add it as a site in Cloudflare, then change the
   nameservers at Squarespace to the two Cloudflare gives. Nothing is served from the domain yet, so
   there is nothing to break.
2. **Create the Pages project**, named `ziman-dev` (the name the workflow passes), as a Direct
   Upload project with no build command: `wrangler pages project create ziman-dev
   --production-branch=main`, or the same in the dashboard.
3. **Give this repository the credentials.** A custom API token with *Account -> Cloudflare Pages ->
   Edit*, and the account id, as the repository secrets `CLOUDFLARE_API_TOKEN` and
   `CLOUDFLARE_ACCOUNT_ID`.
4. **Add the custom domain** `ziman.dev` to the Pages project. With the zone on the same account
   Cloudflare writes the DNS record and issues the certificate itself.
5. **Then remove GitHub Pages**: delete `.github/workflows/pages.yml` and disable the Pages site
   (`gh api -X DELETE repos/vasnatech/ziman/pages`), so there is one live copy and not two.

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
