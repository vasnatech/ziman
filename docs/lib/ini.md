# `ini`

The freedesktop desktop-entry format: groups of `Key=Value`, with localised keys.

**This is the format a Linux desktop is configured in**, and nothing in this library could read
it. `index.theme` in every icon theme, every `.desktop` launcher, `mimeapps.list`, the user
directories file: all of them are this, specified by freedesktop.org's Desktop Entry
Specification (which `index.theme` inherits by reference from the Icon Theme Specification).

**It is not TOML and it is not `properties`, which is why it is its own module.** TOML quotes its
strings, types its values and nests its tables; this has unquoted values, one level of groups,
and a value's type is whatever the key's definition says it is. `properties` has no groups at
all, a different escape set, and `!` as a second comment character. Reading an `index.theme` with
either of them gets the Inherits list wrong -- it is `;`-separated with escapes, not a TOML array
and not one value.

**What is specified and honoured here**: comments (`#`) and blank lines anywhere, group headers
in `[brackets]`, `Key=Value` with the spaces around `=` ignored, **localised keys**
(`Name[de_DE]=...`) resolved by the specification's own fallback order, the five string escapes
(`\s \n \t \r \\`), and `;`-separated lists whose items may contain an escaped `\;`. UTF-8 is the
specified encoding, so it is the default -- the `[byte]` overloads take another for the files
that predate the specification.

**What it refuses rather than guessing**: an entry before the first group header, a group or a
key that occurs twice, and a header that never closes its bracket. The specification forbids all
three; GLib keeps the last duplicate, which means a file with two `Inherits` lines silently gets
one of them, and a reader cannot tell which. A caller for whom a bad file must not be fatal has
the `Err` to act on -- the icon theme reader treats an unreadable `index.theme` as a theme that
is not there, which is the fallback its own specification asks for.

`use ini` -- or `use ini.Name` to bring one item in bare.

## Example

```
use ini
use locale

// The freedesktop desktop-entry format: `index.theme` in every icon theme, every `.desktop`
// launcher, `mimeapps.list`. One level of groups, unquoted values, localised keys, and `;`-separated
// lists -- which is why it is neither TOML nor `properties`.
fn demo() -> Result<unit, string> {
    val file = concat(
        "# Adwaita, trimmed to what a lookup reads\n",
        "[Icon Theme]\n",
        "Name=Adwaita\n",
        "Name[de]=Adwaita\n",
        "Comment=The GNOME icon theme\n",
        "Comment[de]=Das GNOME-Symbolthema\n",
        "Inherits=Adwaita;hicolor;\n",
        "Directories=16x16/actions;scalable/devices\n",
        "\n",
        "[16x16/actions]\n",
        "Size=16\n",
        "Context=Actions\n",
        "Type=Fixed\n",
        "\n",
        "[scalable/devices]\n",
        "Size=128\n",
        "MinSize=8\n",
        "MaxSize=512\n",
        "Type=Scalable\n"
    )
    val theme = try ini.parse(file)
    val top = switch ini.group(theme, "Icon Theme") {
        Some(g) { g }
        None { return err("no [Icon Theme] group") }
    }
    printf("{} groups\n", str(len(theme.groups)))

    // **A list is one value with `;` in it**, and the trailing `;` is a terminator rather than an
    // empty item -- so this is two themes, not three.
    val inherits = ini.list_in(top, "Inherits")
    printf("inherits {} themes, the first is {}\n", str(len(inherits)), inherits[0])

    // A localised key falls back in the specification's order: `lang_COUNTRY@MODIFIER`, then
    // `lang_COUNTRY`, then `lang@MODIFIER`, then `lang`, then the unlocalised key.
    print(switch ini.value_for(top, "Comment", "de_DE") {
        Some(text) { text }
        None { "no comment" }
    })
    print(switch ini.value_for(top, "Comment", "fr_FR") {
        Some(text) { text }
        None { "no comment" }
    })

    // The size directories are groups of their own, named by the `Directories` list.
    for name in ini.list_in(top, "Directories") {
        val g = switch ini.group(theme, name) {
            Some(x) { x }
            None { return err(concat("the Directories list names ", name, ", which is not a group")) }
        }
        printf(
            "{}: size {}, {}\n",
            name,
            str(try ini.get_int(g, "Size")),
            switch ini.value(g, "Type") {
                Some(kind) { kind }
                None { "Threshold" }
            }
        )
    }

    // Writing: `entry_list` escapes each item, so a value holding a `;` reads back as one.
    val written = ini.format(ini.ini_of([
        ini.group_of("Icon Theme", [
            ini.entry_of("Name", "Mine"),
            ini.entry_in("Name", "de", "Meines"),
            ini.entry_list("Inherits", ["Adwaita", "hicolor"])
        ])
    ]))
    print(written)

    // What it refuses, it names -- and all three of these are things the specification forbids
    // rather than things this reader cannot do.
    if Err(why) = ini.parse("Name=before any group\n") {
        print(why)
    }
    if Err(why) = ini.parse("[Icon Theme]\nInherits=a\nInherits=b\n") {
        print(why)
    }
    if Err(why) = ini.parse("[Icon Theme\n") {
        print(why)
    }
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("ini example failed: {}\n", e)
    }
}
```

Output:

```
3 groups
inherits 2 themes, the first is Adwaita
Das GNOME-Symbolthema
The GNOME icon theme
16x16/actions: size 16, Fixed
scalable/devices: size 128, Scalable
[Icon Theme]
Name=Mine
Name[de]=Meines
Inherits=Adwaita;hicolor;

ini: line 1: 'Name' is before any group header
ini: line 3: 'Inherits' occurs twice in group 'Icon Theme', and the specification allows one
ini: line 1: group header '[Icon Theme' never closes its bracket
```

### `Entry`

```
pub struct Entry {
    key: string
    locale: Option<string>
    value: string
}
```

One `Key=Value`, with the locale from a `Key[locale]` if it had one.

**`value` is the text exactly as the file wrote it, escapes and all.** The accessors unescape;
this does not, so `format` can write a file back without inventing or losing an escape. A caller
reading the field directly wants `unescaped` next.

### `Group`

```
pub struct Group {
    name: string
    entries: [Entry]
}
```

### `Ini`

```
pub struct Ini {
    groups: [Group]
}
```

### `default_charset`

```
pub fn default_charset() -> charset.Charset
```

UTF-8, which the specification mandates. The overload taking a `Charset` is for the files that
came before it -- a `.desktop` file from 1999 is Latin-1 and says so nowhere.

### `parse`

```
pub fn parse(text: string) -> Result<Ini, string>
```

```
pub fn parse(data: [byte], set: charset.Charset) -> Result<Ini, string>
```

```
pub fn parse(data: [byte]) -> Result<Ini, string>
```

## Reading

### `group`

```
pub fn group(f: Ini, name: string) -> Option<Group>
```

### `value`

```
pub fn value(g: Group, key: string) -> Option<string>
```

The unlocalised value, unescaped. **`None` and an empty string are different answers**: a key
that is absent is not the same as one set to nothing, and a `.desktop` file uses both.

### `value_for`

```
pub fn value_for(g: Group, key: string, l: Locale) -> Option<string>
```

**The value for a locale, by the specification's own fallback order**: `lang_COUNTRY@MODIFIER`,
then `lang_COUNTRY`, then `lang@MODIFIER`, then `lang`, then the unlocalised key. A POSIX
locale's name carries all three parts (`de_DE.UTF-8@euro`), and the encoding is dropped because a
localised key never names one.

This is the function a user-visible string goes through -- a theme's `Name`, a launcher's
`Comment` -- and `value` is the one for a key whose meaning is not language (`Inherits`, `Size`).

**It matches the locale's POSIX name, which the host may have widened**: `locale.of("de")` opens
`de_DE.UTF-8` on a host where that is what German means, so a file carrying `Name[de_DE]` and no
`Name[de]` answers with the German of Germany. A caller who means the tag it typed and nothing
else passes the tag instead of a `Locale`.

```
pub fn value_for(g: Group, key: string, tag: string) -> Option<string>
```

The same from a locale tag as a file or an environment variable writes one -- `de_DE@euro`,
`pt_BR`, `sr@latin`, with or without a `.UTF-8` in the middle. This is the door for a caller
reading `LC_MESSAGES` itself, and for a test that means exactly what it says.

### `list_of`

```
pub fn list_of(text: string) -> [string]
```

A `;`-separated list, the specification's own multi-value shape: `Inherits=Adwaita;hicolor;`.

**A trailing `;` is a terminator, not an empty item**, which the specification says and which
every real file relies on -- `Adwaita;hicolor;` is two themes. An item may contain a `\;`.

### `list_in`

```
pub fn list_in(g: Group, key: string) -> [string]
```

The list a key holds, or an empty list when the key is absent -- which is the same answer a
caller wants in both cases, since a theme with no `Inherits` inherits nothing.

### `get_bool`

```
pub fn get_bool(g: Group, key: string) -> Result<bool, string>
```

`true` or `false`, and nothing else: the specification names those two spellings, so `True`,
`yes` and `1` are refusals rather than guesses -- a `Hidden=1` that read as true would hide a
launcher the file did not hide.

### `get_int`

```
pub fn get_int(g: Group, key: string) -> Result<int, string>
```

### `get_float`

```
pub fn get_float(g: Group, key: string) -> Result<float, string>
```

### `get_string`

```
pub fn get_string(g: Group, key: string) -> Result<string, string>
```

## Escapes

### `unescaped`

```
pub fn unescaped(text: string) -> string
```

The five escapes the specification defines, and **only** those five: `\s` is a space, `\n` a
newline, `\t` a tab, `\r` a carriage return and `\\` a backslash. A backslash before anything
else is a backslash, because a file that writes a Windows path as `C:\temp` is not written in
escapes and refusing it would be refusing the file over a convention it never adopted.

### `escaped`

```
pub fn escaped(text: string) -> string
```

The inverse, for a value going into a file: **the five, and only the five**, so that
`unescaped(escaped(x))` is `x` for every x. A `;` is not escaped here -- it is only a separator
inside a list, and `list_text` is what escapes it.

## Writing

### `format`

```
pub fn format(f: Ini) -> string
```

The data back as a file. **Comments and blank lines are not data and do not survive**, which is
the same trade `json.format` and `toml.format` make: what round-trips is the entries, and an
`Entry.value` is the raw text so no escape is invented or lost on the way through.

### `list_text`

```
pub fn list_text(items: [string]) -> string
```

Several values as one value: each item escaped, `;` between them, and a `;` at the end because
that is how every file in the wild terminates a list (and `list_of` reads either).

### `entry_of`

```
pub fn entry_of(key: string, value: string) -> Entry
```

A group built in code, for a caller writing a file rather than reading one.

### `entry_in`

```
pub fn entry_in(key: string, l: string, value: string) -> Entry
```

### `entry_list`

```
pub fn entry_list(key: string, items: [string]) -> Entry
```

A list-valued entry, which is `entry_of` plus `list_text`: `Inherits`, `MimeType`, `Categories`
and `OnlyShowIn` are all this shape, and writing one by hand is how a `;` ends up unescaped.

### `group_of`

```
pub fn group_of(name: string, entries: [Entry]) -> Group
```

### `ini_of`

```
pub fn ini_of(groups: [Group]) -> Ini
```
