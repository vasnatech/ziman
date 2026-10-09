# `regex`

Regular expressions: a native engine that runs in time linear in the text (no
backreferences), with patterns compiled once and cached.


```
val re = try regex.compile("(\\w+)@(\\w+)\\.com")
val m = regex.find(re, "mail bob@site.com now")     // Option<Match>
regex.replace_all(re, text, "$2: $1")
```

A Perl-flavoured syntax, without backreferences or lookaround -- which is what
lets every match run in time proportional to (text length x pattern size), so
a pattern that comes from outside can never make a program hang. Text and
patterns are UTF-8; every offset is a *byte* offset, like `substring`.

Supported: literals, `.`, `[a-z]` `[^...]` classes (with `\d \w \s`, POSIX
`[:alpha:]` and `\p{...}` inside), `\d \D \w \W \s \S` (ASCII), `\p{L}` /
`\P{L}` Unicode categories and scripts, `\b \B`, `^ $`, groups `( )`,
`(?: )` and named `(?P<name>...)` / `(?<name>...)`,
alternation `|`, and the quantifiers `* + ? {n} {n,} {n,m}` (greedy, or lazy
with a trailing `?`). Escapes: `\n \r \t \f \v \0 \xHH` and `\` before any
punctuation. Flags go first: `(?i)` ignore ASCII case, `(?s)` `.` matches
newline, `(?m)` `^`/`$` match at line breaks (combine: `(?im)`). `.` and
classes match one whole UTF-8 character. `$` matches only at the very end
unless `(?m)`. Matching is leftmost, with Perl's priority between
alternatives.

`use regex` -- or `use regex.Name` to bring one item in bare.

## Example

```
use regex

fn main() {
    val re = unwrap(regex.compile("(\\w+)@(\\w+)\\.com"))
    print(Option.is_Some(regex.find(re, "mail bob@site.com now")))
    print(regex.replace_all(re, "bob@site.com, amy@corp.com", "$2: $1"))
    print(is_ok(regex.compile("(unclosed")))
}
```

Output:

```
true
site: bob, corp: amy
false
```

### `Regex`

```
pub struct Regex {
    handle: int
    groups: int
    pattern: string
    names: [string]
}
```

(A pattern compiled before is looked up, not recompiled: the compiler keeps the last 256
distinct patterns, so `compile` in a loop or on every request is cheap.)

A compiled pattern. Immutable and shareable between threads; compile once.

### `Match`

```
pub struct Match {
    start: int
    end: int
    text: string
    groups: [Option<string>]
    names: [string]
}
```

One match: where it is (byte offsets, `end` exclusive), its text, and the
capture groups 1..n in order (`None` for a group that did not take part).

### `compile`

```
pub fn compile(pattern: string) -> Result<Regex, string>
```

Compiles `pattern`. An Err usually names the position of the problem -- except when the pattern is
refused for its size, where it names the size instead: a pattern needing more than 64 MB of
capture buffers to match is refused here rather than attempted later, which in practice is a cap
of about 1,170 capture groups and never one on pattern length.

(A pattern compiled before is looked up, not recompiled: the last 256 distinct patterns are kept,
so `compile` in a loop or on every request is cheap -- for patterns that compile. A *failed*
compile is not cached, so a caller that recompiles a bad pattern per request pays for it every
time.)

### `is_match`

```
pub fn is_match(re: Regex, text: string) -> bool
```

True if the pattern matches anywhere in `text`.

### `find_from`

```
pub fn find_from(re: Regex, text: string, from: int) -> Option<Match>
```

The first match at or after byte offset `from` (0 <= from <= len(text)).

### `find`

```
pub fn find(re: Regex, text: string) -> Option<Match>
```

The first match in `text`, or None.

### `find_all`

```
pub fn find_all(re: Regex, text: string) -> [Match]
```

Every non-overlapping match, left to right. After an empty match the scan
moves on by one character.

### `group`

```
pub fn group(m: Match, n: int) -> Option<string>
```

The text of group `n` of a match: 0 is the whole match, 1.. the groups.
`None` for a group that did not take part or one that does not exist.

### `group_named`

```
pub fn group_named(m: Match, name: string) -> Option<string>
```

The text of the group written `(?P<name>...)` or `(?<name>...)`. `None` if it
did not take part or no group has that name.

### `replace`

```
pub fn replace(re: Regex, text: string, replacement: string) -> string
```

Replaces the first match; see `expand` for `$1`-style references.

### `replace_all`

```
pub fn replace_all(re: Regex, text: string, replacement: string) -> string
```

Replaces every non-overlapping match; see `expand` for `$1`-style references.

### `split`

```
pub fn split(re: Regex, text: string) -> [string]
```

The pieces of `text` between matches (an empty match separates nothing).

### `escape`

```
pub fn escape(text: string) -> string
```

`text` with every regex metacharacter escaped, so it matches itself.
