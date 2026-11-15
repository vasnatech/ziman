# `icons`

The desktop's own icons, by name.

**`icons.icon_path(theme, "document-save", 16)` is the point of this module**, and it is the last
step of the icon work: `widget.IconSource.Drawn` takes a path in a unit box, `svg` reads a file
into one, and this is what turns a *name* into the file. An application that draws its own
artwork looks imported on a desktop; one that asks the theme for `"document-save"` looks like it
belongs, follows the user's theme when they change it, and inherits the accessibility work the
theme already did.

**This is a filesystem problem, not a graphics one**, which is why it reads as a search rather
than as drawing: freedesktop.org's Icon Theme Specification defines a search path, an
`index.theme` per theme (`ini` reads those), a list of size directories with a matching rule, and
an inheritance chain ending at `hicolor`. The algorithm here is the specification's, and the
private functions are named after its own -- `matches_size` is `DirectoryMatchesSize`,
`size_distance` is `DirectorySizeDistance` -- so the two can be read side by side.

**What a lookup answers is a FILE, not a drawing.** A theme directory holds `.png` as well as
`.svg`, and only an SVG becomes a tintable `shape.Path` -- so `find` answers whatever the
specification picks (PNG first, as it says), `find_svg` answers only what `svg` can read, and
`icon_path` is `find_svg` plus `svg`. A caller who wants the PNG wants `image.decode_png` and a
`scene.image`: a decoded picture does not tint and needs one file per state, which is the whole
reason the drawn source exists.

**Measured end to end against the desktop it was written for**, because a lookup that reads its
own fixtures proves nothing about a real theme. Adwaita's **585 symbolic icons**, each asked for
by name at 16 pixels and taken all the way to a unit-box path:


```
found by name                585
read exactly                  495
read dropping the faint       578
```

The 90 that need the tolerant door are two-tone icons -- a solid outline over a fill at 0.35 --
and the 7 that neither door reads mix fill rules or carry a clip path (see `svg`'s own header for
why that is arithmetic rather than scope). **Getting that far needed one fix nothing in the
specification hints at**: the Icon Theme Specification separates `Inherits` and `Directories`
with COMMAS where everything else freedesktop writes uses `;`, so the first version of this
module found a chain of two themes with zero directories between them and not one icon.

`use icons` -- or `use icons.Name` to bring one item in bare.

## Example

```
use fs
use icons
use shape

// The desktop's own icons, by name. On a real machine this is `icons.theme("Adwaita")` and then
// `icons.icon_path(theme, "document-save-symbolic", 16)`; here it is a two-theme tree written to a
// temporary directory, because an example has to print the same thing on every machine.
val root: string = concat(fs.temp_dir(), "/ziman-icons-example/icons")

fn build() -> Result<unit, string> {
    // A parent with three sizes, and a child that has only one and inherits the rest.
    try put(concat(root, "/example-base/index.theme"), concat(
        "[Icon Theme]\n",
        "Name=Base\n",
        // **Commas**: the Icon Theme Specification separates its lists with them, where the
        // Desktop Entry Specification it otherwise borrows says `;`.
        "Directories=16x16/apps,22x22/apps,scalable/apps\n",
        "\n[16x16/apps]\nSize=16\nType=Fixed\n",
        "\n[22x22/apps]\nSize=22\nType=Fixed\n",
        "\n[scalable/apps]\nSize=128\nMinSize=24\nMaxSize=512\nType=Scalable\n"
    ))
    try put(concat(root, "/example-base/16x16/apps/save.png"), "a png")
    try put(concat(root, "/example-base/22x22/apps/save.png"), "a png")
    try put(concat(root, "/example-base/scalable/apps/save.svg"), tick())
    try put(concat(root, "/example-child/index.theme"), concat(
        "[Icon Theme]\n",
        "Name=Child\n",
        "Inherits=example-base\n",
        "Directories=16x16/apps\n",
        "\n[16x16/apps]\nSize=16\nType=Fixed\n"
    ))
    try put(concat(root, "/example-child/16x16/apps/save.png"), "a png")
    return unit
}

fn put(path: string, text: string) -> Result<unit, string> {
    if Err(e) = fs.make_dirs(fs.parent_dir(path)) {
        return err(fs.fs_error_message(e))
    }
    if Err(e) = fs.write_text(path, text) {
        return err(fs.fs_error_message(e))
    }
    return unit
}

fn tick() -> string {
    return "<svg viewBox=\"0 0 24 24\"><path d=\"M9 16.2L4.8 12l-1.4 1.4L9 19 21 7l-1.4-1.4z\"/></svg>"
}

fn under(path: string) -> string {
    if starts_with(path, concat(root, "/")) {
        return substring(path, len(root) + 1, len(path))
    }
    return path
}

fn found(t: icons.Theme, name: string, size: int) -> string {
    return switch icons.find(t, name, size) {
        Some(f) { concat(under(f.path), " (from a ", str(f.size), "-pixel directory)") }
        None { "not found" }
    }
}

fn demo() -> Result<unit, string> {
    try build()
    // On a desktop: `icons.theme("Adwaita")`, which reads the search path from the environment.
    // The overload taking a path is for a program that ships its own theme -- or an example.
    val theme = try icons.theme("example-child", [root])
    val names = mutable_string_new()
    for indexed in theme.chain {
        append(names, concat(indexed.name, " "))
    }
    printf("chain: {}\n", trim(snapshot(names)))

    // **Two passes, and the first one is what makes this look right.** Asked for 16, the child
    // answers: both themes have a 16-pixel directory and the child comes first. Asked for 22 the
    // *parent* answers, because a directory whose rule serves the size beats a theme that is
    // nearer in the chain -- a stretched icon looks worse than a slightly different drawing.
    printf("16: {}\n", found(theme, "save", 16))
    printf("22: {}\n", found(theme, "save", 22))
    // 64 is inside the scalable directory's range, so it matches rather than falling back.
    printf("64: {}\n", found(theme, "save", 64))
    // 13 matches nothing -- the fixed sizes are 16 and 22 and the scalable one starts at 24 -- so
    // the closest-size pass decides, and 16 is three away where 22 is nine.
    printf("13: {}\n", found(theme, "save", 13))

    // A PNG comes before an SVG, which is the specification's order: a theme that ships both drew
    // the PNG for that size. A caller that wants a *path* to tint asks for the scalable one.
    printf("svg at 16: {}\n", switch icons.find_svg(theme, "save", 16) {
        Some(f) { under(f.path) }
        None { "not found" }
    })

    // And the whole of it: a name becomes the path `widget.IconSource.Drawn` takes.
    val drawn = try icons.icon_path(theme, "save", 64)
    val box = shape.path_bounds(drawn)
    printf(
        "drawn: {} segments in {:.2},{:.2} {:.2}x{:.2}\n",
        str(len(drawn.segments)),
        box.x,
        box.y,
        box.width,
        box.height
    )

    // A name no theme in the chain has says so, naming the theme that was asked.
    if Err(why) = icons.icon_path(theme, "no-such-icon", 16) {
        print(why)
    }
    if Err(e) = fs.remove_all(concat(fs.temp_dir(), "/ziman-icons-example")) {
        return err(fs.fs_error_message(e))
    }
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("icons example failed: {}\n", e)
    }
}
```

Output:

```
chain: example-child example-base
16: example-child/16x16/apps/save.png (from a 16-pixel directory)
22: example-base/22x22/apps/save.png (from a 22-pixel directory)
64: example-base/scalable/apps/save.svg (from a 128-pixel directory)
13: example-child/16x16/apps/save.png (from a 16-pixel directory)
svg at 16: example-base/scalable/apps/save.svg
drawn: 7 segments in 0.14,0.23 0.73x0.56
icons: no scalable 'no-such-icon' at 16 in theme 'example-child' or anything it inherits
```

### `Kind`

```
pub enum Kind {
    // One size only: `16x16/actions` holds 16-pixel icons and nothing else.
    Fixed
    // A range, which is what a `scalable/` directory of SVGs is.
    Scalable
    // A nominal size that stretches by `Threshold` either way. **This is the default**, which
    // matters: a directory section with a `Size` and nothing else is a threshold directory with a
    // threshold of 2, not a fixed one.
    Threshold
}
```

How a directory's `Size` relates to the sizes it serves.

### `SizeDir`

```
pub struct SizeDir {
    // Relative to the theme's own directory: `16x16/actions`, `scalable/devices`.
    path: string
    size: int
    scale: int
    kind: Kind
    min: int
    max: int
    threshold: int
}
```

One size directory of a theme, as its `index.theme` section describes it.

### `Indexed`

```
pub struct Indexed {
    name: string
    roots: [string]
    dirs: [SizeDir]
}
```

One theme's index: its name, every base directory that holds a copy of it, and its size
directories.

**`roots` is a list because a theme can be spread across the search path**: a user's own
`~/.local/share/icons/Adwaita` adds to the system one rather than replacing it, and the
specification's lookup walks every base directory for each size directory in turn.

### `Theme`

```
pub struct Theme {
    asked: string
    // The search path this was resolved with, so every later lookup walks the same directories the
    // chain came from. **Carried rather than re-read**: `search_path()` is a function of the
    // environment, and a theme that answered one way at startup must not answer another way later
    // because something changed `XDG_DATA_DIRS` -- which is also what lets a caller (or a test)
    // point this at a directory of its own.
    path: [string]
    chain: [Indexed]
}
```

A theme and the chain behind it, resolved once: the theme itself, then its parents in order,
then `hicolor`, which every theme inherits whether it says so or not.

### `Found`

```
pub struct Found {
    path: string
    // The nominal size of the directory it came from, which is not the size asked for: a 16-pixel
    // request can land in a 22-pixel directory when that is the closest a theme has.
    size: int
    scalable: bool
}
```

Where a name resolved to.

### `search_path`

```
pub fn search_path() -> [string]
```

### `theme`

```
pub fn theme(name: string) -> Result<Theme, string>
```

A theme and everything behind it, ready to answer names.

**A theme whose `index.theme` is unreadable is a theme that is not there**, which is the
specification's own fallback behaviour and the reason this does not fail: the chain simply
continues to the parents and to `hicolor`. The `Err` is for the one case a caller cannot recover
from -- *nothing* on the search path, not even `hicolor`, which means the machine has no icon
theme installed at all.

```
pub fn theme(name: string, path: [string]) -> Result<Theme, string>
```

With a search path of its own, for a caller that ships a theme beside its program -- or a test
that needs the answer to hold still.

## The lookup

### `find`

```
pub fn find(t: Theme, name: string, size: int) -> Option<Found>
```

`FindIcon`: the file for this name at this size, or `None`.

**Two passes, in this order, which is the part a reader invents wrongly**: every directory whose
rule *matches* the size, in theme order; and only if none does, the directory whose size is
*closest*. So a 16-pixel request prefers a 16-pixel directory in a parent theme over a 22-pixel
one in the theme itself -- the size the caller asked for matters more than the theme it comes
from, because a stretched icon looks worse than a slightly different drawing.

```
pub fn find(t: Theme, name: string, size: int, scale: int) -> Option<Found>
```

With a display scale: a 16-pixel icon on a 2x display wants the 32-pixel artwork, and a theme
that ships both says so with `Scale`.

### `find_svg`

```
pub fn find_svg(t: Theme, name: string, size: int) -> Option<Found>
```

Only the files `svg` can read, for a caller that wants a tintable path rather than a picture.
The same two passes, over one extension.

```
pub fn find_svg(t: Theme, name: string, size: int, scale: int) -> Option<Found>
```

## A name as a drawing

### `icon_path`

```
pub fn icon_path(t: Theme, name: string, size: int) -> Result<shape.Path, string>
```

**The whole of it: a name, and the path to draw.** Looks up the SVG, reads it, merges what it
paints and scales that into the unit box `widget.IconSource.Drawn` wants.

This is the exact reading, so it refuses a file one path cannot express -- a gradient fill, or a
part that is not fully opaque, which a fifth of Adwaita is. The overload taking a tolerance is
the one a real desktop wants; see `svg.solid_path` for what it gives up.

```
pub fn icon_path(t: Theme, name: string, size: int, least: float) -> Result<shape.Path, string>
```

The same, dropping every part below `least` opacity rather than refusing it. `1.0` keeps only
what is fully opaque, which is what reads 917 of a desktop's 1,206 icons where the exact door
reads 718.
