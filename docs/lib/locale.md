# `locale`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

A `Locale`, and the process-wide default that locale-aware functions use when given
none.

A `Locale` is the operating system's, opened by name: `locale.of("tr_TR")`. The data is the
host's, not a table in this repository, which was measured rather than assumed on 2026-10-23 --
glibc answers correct month and day names, decimal and grouping separators, per-locale case
mapping (including the Turkish dotted/dotless `i`) and real collation for every locale the host
has generated, and this machine had 890 of them. docs/ROADMAP.md's Locale entry carries the
measurements and the limits.

**What this cannot do**, so it is said once here rather than discovered: one-to-many case
mapping. `towupper_l` maps one character to one character, so German `ß` does not become `SS`
and the Greek final sigma is not contextual. Those need ICU or a bundled table; everything else
the OS answers correctly.

**Three rules decide what a Locale is for:**


```
1. A locale-aware function with no `Locale` argument uses `locale.default()`.
2. The default is `locale.root()` until something sets it. Root is the invariant locale: ASCII
   case folding, code-point order, `.` for a decimal point. It is **not** the host's locale,
   deliberately -- the same program would otherwise render differently on a Turkish laptop and
   an English server, with nothing in the code to show it. `locale.use_system()` adopts the
   host's in one explicit line.
3. Protocol and identifier code passes `locale.root()` explicitly. That is not a style
   preference: Turkish `to_lower("ID")` is `ıd`, so a header match, a hex digit, a FHIR code
   or an environment-variable name folded under a user's locale is a bug. `locale.root()` is the
   name for "fold case the way a wire format means it".
```

`of` never fails. A name the host cannot provide resolves to root, and `name_of` reports what was
actually opened -- so a miss is visible without `of` answering a `Result` that most callers would
only unwrap.

`use locale` -- or `use locale.Name` to bring one item in bare.

### `of`

```
pub fn of(asked: string) -> Locale
```

The locale named `asked`, or root when the host has no such locale.

Accepts `tr_TR`, `tr-TR`, `tr`, `en_US.UTF-8` and `C`. The codeset is forced to UTF-8 where the
name does not state one, because a bare `tr_TR` resolves to Latin-5 on glibc and its month names
would then not be valid UTF-8.

### `name_of`

```
pub fn name_of(l: Locale) -> string
```

The POSIX name this locale actually opened: `of("tr")` is `tr_TR.UTF-8`, and `of("xx")` is
whichever root the host has. Comparing it against what was asked is how a caller detects a
fallback, `of` having no Err to carry one.

### `root`

```
pub fn root() -> Locale
```

**The invariant locale**: ASCII case folding, code-point order, a `.` decimal point, English
names. What protocol and identifier code should pass, and what the process default is until
something changes it.

A function rather than a `val` because a module-level `val` can only be reached through
`use locale.root`, which would put a bare `root` into every file that folds a header or a hex
digit. The handle underneath is opened once and shared, so calling this in a loop costs a native
call rather than a locale open -- though a per-character loop should still hoist it into a `val`.

### `system`

```
pub fn system() -> Locale
```

What the environment asks for (`LC_ALL`, then `LC_CTYPE`, then `LANG`), or root when it asks for
nothing. Reading this does not make it the default -- `use_system` does that, in one line a
reader can see.

### `default`

```
pub fn default() -> Locale
```

The process-wide locale that a locale-aware function uses when given none. Root until set.

### `set_default`

```
pub fn set_default(l: Locale)
```

Sets the process-wide default. **Process-wide and meant for startup**: one write, read by every
locale-aware call on every thread, so changing it mid-flight changes what other threads are in
the middle of rendering. A request's own locale belongs in an argument, not here.

### `use_system`

```
pub fn use_system() -> Locale
```

Adopts the host's locale as the process default, which is the one line that lets a machine's
configuration reach a program's output. Answers what it set, so a caller can log it.

### `month_name`

```
pub fn month_name(l: Locale, month: int, full: bool) -> string
```

The locale's name for month `month` (1-12): the full name for `full`, the abbreviation otherwise.
`Ocak`/`Oca` under `tr_TR`, `Januar`/`Jan` under `de_DE`, `1月` under `ja_JP`.

**Not a fixed width.** Month names differ in length within a locale and between locales, which is
why `DateTimeFormat.width` is -1 for any pattern that renders one.

Some abbreviated names arrive padded for column alignment -- glibc's `ja_JP` set is " 1月" -- and
are trimmed, which is this library altering the host's data and so is said rather than left to be
noticed in output.

### `day_name`

```
pub fn day_name(l: Locale, day: int, full: bool) -> string
```

The locale's name for a day of the week, `day` being 0 for Sunday through 6 for Saturday -- POSIX
numbers them that way (`DAY_1` is Sunday) and so does this, rather than rotating twice.

### `decimal_separator`

```
pub fn decimal_separator(l: Locale) -> string
```

The locale's decimal point: `.` under `en_US`, `,` under `tr_TR` and `de_DE`.

`number.number_format_for(l)` is the usual way to reach this; it is public because a caller
writing their own formatter needs the same two characters.

### `thousands_separator`

```
pub fn thousands_separator(l: Locale) -> string
```

The locale's digit-grouping separator: `,` under `en_US`, `.` under `tr_TR`, a space under
`sv_SE`, and **empty under `ar_SA`**, which groups with nothing. Empty is a real answer here and
not a failure.
