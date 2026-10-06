# `url`

The percent-encoding both `http` and `db` need, in one place.

It began as two copies of the same loop: `http.percent_decode` and `db.percent_decode`, differing only in
whether `+` counts as a space and in which error type they returned. Sharing it by having one call the
other was not an option worth taking -- `db` does not depend on `http` and should not start, for a
`%XX` loop -- so the algorithm lives here, below both of them, and each keeps its own signature as a thin
adapter: `http.percent_decode` for the public API that already existed, `db.percent_decode` to answer a
`DbError` the way the rest of that module does.

What is deliberately NOT here is URL *parsing*. `http.client.parse_url` and `db.parse_url` look like
duplicates at 80 and 82 lines, and are not: they read different grammars into different shapes
(`Url { secure, host, port, target, userinfo }` against
`DbUrl { user, password, host, port, database, params }`), for a `https://host/path` and a
`postgres://user:pw@host/db?a=b` respectively. Merging them would mean inventing a general URL type for
both to specialise, which is a design change rather than a de-duplication.

`use url` -- or `use url.Name` to bring one item in bare.

### `percent_decode`

```
pub fn percent_decode(s: string, plus_is_space: bool) -> Result<string, string>
```

Decodes `%XX` escapes in `s`. `plus_is_space` is for query strings, where `a+b` means `a b` -- in a path a
`+` is a literal plus. An Err for a truncated or malformed escape, or one that would put a NUL byte in a
string (which Ziman strings cannot hold).

### `percent_encode`

```
pub fn percent_encode(s: string, plus_for_space: bool) -> string
```

The inverse of `percent_decode`: every byte except the unreserved characters (letters, digits,
`-` `_` `.` `~`) as `%XX` (uppercase hex). With `plus_for_space` a space is written `+` (form and query
encoding) instead of `%20`.
