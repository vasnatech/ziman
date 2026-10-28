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

## Example

```
use url

// Percent-encoding, both directions. The pair matters more than either half: text that survives a
// round trip through a URL is text the other side reads back unchanged, and the two rules that
// differ between a path and a query string -- whether `+` means a space -- are a parameter here
// rather than something each caller remembers differently.
fn demo() -> Result<unit, string> {
    // In a path segment, a space is `%20` and `+` is a literal plus.
    print(url.percent_encode("a b&c/e", false))
    print(try url.percent_decode("a%20b%26c%2Fe", false))

    // In a query string it is `+`, which is why decoding has to be told which it is reading.
    print(url.percent_encode("a b&c/e", true))
    print(try url.percent_decode("a+b%26c", true))
    print(try url.percent_decode("a+b%26c", false))

    // Non-ASCII goes through as its UTF-8 bytes, so the round trip is the text it started as.
    val accented = "naïve café"
    val encoded = url.percent_encode(accented, false)
    print(encoded)
    print(try url.percent_decode(encoded, false) == accented)

    // A malformed escape is an Err, not a guess at what was meant.
    if Err(e) = url.percent_decode("100%zz", false) {
        print(e)
    }
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("url example failed: {}\n", e)
    }
}
```

Output:

```
a%20b%26c%2Fe
a b&c/e
a+b%26c%2Fe
a b&c
a+b&c
na%C3%AFve%20caf%C3%A9
true
bad %-escape
```

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
