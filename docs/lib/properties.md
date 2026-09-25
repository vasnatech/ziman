# `properties`

Spring-Boot-style configuration: `key=value` files,
profiles, `${placeholder}` substitution, and environment-variable
overrides, all reporting errors that name the key and where its value
came from. Pure Ziman on top of `io` and `os`.

Format: one `key=value` per line; `#` starts a comment only at the
beginning of a line; keys are lowercase, dotted, kebab-case
(`server.idle-timeout-ms`); values are trimmed, with no quoting,
escapes or inline comments. Anything else -- a duplicate key, an
uppercase key, a line that is neither blank, comment nor key=value --
is an error with its file:line, never silently skipped.

Loading `load("application")` reads `application.properties` (required)
from the working directory, then `application-<profile>.properties`
when a profile is active. The profile is env `PROFILE_ACTIVE`, else
`profile.active` in the base file; an active profile whose file is
missing is an error (it catches typos). Profile values override base
values.

Lookup precedence, highest first: environment variable (the key
upper-cased with `.`/`-` -> `_`, so `server.port` is `SERVER_PORT`),
profile file, base file. Values may contain `${name}` or
`${name:default}`, resolved against properties, then the environment
(exact name), then the default; an unresolvable placeholder or a
cycle is an error. The default may itself contain `:`.

Placeholders are resolved once, when the properties are built, so a bad
one fails startup and every lookup afterwards is a plain array scan.

`use properties` -- or `use properties.Name` to bring one item in bare.

### `Entry`

```
pub struct Entry {
    key: string
    value: string
    source: string
}
```

One setting: its `key`, its `value`, and `source`, where it came from (a file, the environment,
an override) for messages that say why a value is what it is.

### `Properties`

```
pub struct Properties {
    entries: [Entry]
    profile: string
}
```

The loaded configuration: every entry in lookup order, and the active `profile` (empty when
none).

### `lookup`

```
pub fn lookup(props: Properties, key: string) -> Option<Entry>
```

The entry for `key`: its environment variable if set (taken literally),
else the file entry.

### `parse`

```
pub fn parse(text: string, source: string) -> Result<Properties, string>
```

Parses one properties text, resolving placeholders within it.

### `load`

```
pub fn load(name: string) -> Result<Properties, string>
```

Reads `<name>.properties` from the working directory (an Err if it is missing), then
`<name>-<profile>.properties` if a profile is active (env `PROFILE_ACTIVE`, else `profile.active` in
the base file; an active profile with no file is an Err), applies the profile's values over the
base ones, and resolves every `${placeholder}`. The result is what `lookup` and the `get_*`
functions read.

### `entry_string`

```
pub fn entry_string(e: Entry) -> Result<string, string>
```

The entry's value as a string (always Ok; here so every `entry_*` has the same shape, which is
what `#[derive(Properties)]` relies on).

### `entry_int`

```
pub fn entry_int(e: Entry) -> Result<int, string>
```

The entry's value as an int. An Err naming the key, the text and where the value came from.

### `entry_float`

```
pub fn entry_float(e: Entry) -> Result<float, string>
```

The entry's value as a float. An Err naming the key, the text and where the value came from.

### `entry_bool`

```
pub fn entry_bool(e: Entry) -> Result<bool, string>
```

The entry's value as a bool: `true` or `false`, any case. An Err otherwise, naming the key and source.

### `get_string`

```
pub fn get_string(props: Properties, key: string) -> Result<string, string>
```

Required typed getters. An optional key is `lookup` plus `entry_*`
(what #[derive(Properties)] generates for an Option<T> field).

The value of the required property `key` (environment variable first, then the files) as a
string. An Err if it is missing; the message names the key and the environment variable.

### `get_int`

```
pub fn get_int(props: Properties, key: string) -> Result<int, string>
```

The value of the required property `key` (environment variable first, then the files) as an
int. An Err if it is missing or not an integer; the message names the key and where to set it.

### `get_float`

```
pub fn get_float(props: Properties, key: string) -> Result<float, string>
```

The required property `key` as a float; same lookup and errors as `get_int`.

### `get_bool`

```
pub fn get_bool(props: Properties, key: string) -> Result<bool, string>
```

The required property `key` as a bool (`true` / `false`); same lookup and errors as `get_int`.

### `key`

```
pub fn key(prefix: string, name: string) -> string
```

"server" + "port" -> "server.port"; an empty prefix leaves the name alone.

### `check_unknown`

```
pub fn check_unknown(props: Properties, known: [string]) -> Result<bool, string>
```

Fails on a key in the files that isn't in `known` (`profile.active` is
always allowed), so a typo like `server.prot` stops startup instead of
silently falling back to a default.

### `report`

```
pub fn report(props: Properties, keys: [string]) -> string
```

One line per key in `keys` that has a value: `key=value (source)`,
with secrets masked. Meant to be printed at startup.
