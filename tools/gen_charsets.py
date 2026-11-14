#!/usr/bin/env python3
"""Generates lib/core/charset/tables.zm: the single-byte character sets, as data.

**Generated rather than written, which is the whole point.** A single-byte set is a 128-entry
table and nothing else, and a table written from memory is wrong in a way no review catches --
the reason `truetype` read Macintosh font names as Latin-1 for a day with a comment saying so.
The tables come from Python's own codec data, which is CPython's vendored copy of the Unicode
consortium's mappings, so this needs no network and no vendored files of its own.

**Not resource files**, which was the first idea: `charset` cannot read a file, because `fs` uses
`charset` and the loader refuses a circular `use`. A caller who wants a set nobody shipped builds
one with `charset.single_byte(name, upper)` from a table it read however it likes -- the
filesystem dependency belongs in the application, not under every text boundary in the library.

Run with `make charsets`; `make charsets-check` fails when the committed file is not what this
emits, the same gate every other generated file here has.
"""
import collections
import encodings.aliases
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "lib", "core", "charset", "tables.zm")

# **Latin-1 and CP1252 are deliberately absent**: they are `Charset.Latin1` and `Charset.WinAnsi`,
# hand-written because one is arithmetic (every byte is the code point of the same number) and the
# other is Latin-1 plus a 32-entry block. A second copy as a table would be a second answer.
SETS = [
    ("iso_8859_2", "iso8859_2", "ISO-8859-2", "central European"),
    ("iso_8859_3", "iso8859_3", "ISO-8859-3", "south European, Maltese and Esperanto"),
    ("iso_8859_4", "iso8859_4", "ISO-8859-4", "north European"),
    ("iso_8859_5", "iso8859_5", "ISO-8859-5", "Cyrillic"),
    ("iso_8859_6", "iso8859_6", "ISO-8859-6", "Arabic"),
    ("iso_8859_7", "iso8859_7", "ISO-8859-7", "Greek"),
    ("iso_8859_8", "iso8859_8", "ISO-8859-8", "Hebrew"),
    ("iso_8859_9", "iso8859_9", "ISO-8859-9", "Turkish -- Latin-1 with the six Turkish letters"),
    ("iso_8859_10", "iso8859_10", "ISO-8859-10", "Nordic"),
    ("iso_8859_11", "iso8859_11", "ISO-8859-11", "Thai"),
    ("iso_8859_13", "iso8859_13", "ISO-8859-13", "Baltic rim"),
    ("iso_8859_14", "iso8859_14", "ISO-8859-14", "Celtic"),
    ("iso_8859_15", "iso8859_15", "ISO-8859-15", "Latin-1 with the euro sign"),
    ("iso_8859_16", "iso8859_16", "ISO-8859-16", "south-eastern European"),
    ("cp1250", "cp1250", "windows-1250", "central European"),
    ("cp1251", "cp1251", "windows-1251", "Cyrillic"),
    ("cp1253", "cp1253", "windows-1253", "Greek"),
    ("cp1254", "cp1254", "windows-1254", "Turkish"),
    ("cp1255", "cp1255", "windows-1255", "Hebrew"),
    ("cp1256", "cp1256", "windows-1256", "Arabic"),
    ("cp1257", "cp1257", "windows-1257", "Baltic rim"),
    ("cp1258", "cp1258", "windows-1258", "Vietnamese"),
    ("koi8_r", "koi8_r", "KOI8-R", "Russian"),
    ("koi8_u", "koi8_u", "KOI8-U", "Ukrainian"),
    ("mac_roman", "mac_roman", "Mac Roman", "the Macintosh platform's own, and what a TrueType platform-1 name record is in"),
]

# The aliases a set is written with in a header or a declaration, from Python's own table, plus
# the short forms nobody spells in full.
EXTRA = {
    "iso_8859_2": ["latin2", "l2"],
    "iso_8859_3": ["latin3", "l3"],
    "iso_8859_4": ["latin4", "l4"],
    "iso_8859_5": ["cyrillic"],
    "iso_8859_6": ["arabic", "asmo708"],
    "iso_8859_7": ["greek", "greek8"],
    "iso_8859_8": ["hebrew"],
    "iso_8859_9": ["latin5", "l5"],
    "iso_8859_10": ["latin6", "l6"],
    "iso_8859_11": ["thai"],
    "iso_8859_13": ["latin7", "l7"],
    "iso_8859_14": ["latin8", "l8"],
    "iso_8859_15": ["latin9", "l9"],
    "iso_8859_16": ["latin10", "l10"],
    "mac_roman": ["macintosh", "macroman"],
}


def squeezed(name):
    """The same folding `charset.charset_named` does: lower case, no separators."""
    return "".join(c for c in name.lower() if c not in "-_ .")


def table_of(codec):
    """The 128 code points for 0x80..0xFF, with -1 where the set has no character."""
    out = []
    for b in range(0x80, 0x100):
        try:
            out.append(ord(bytes([b]).decode(codec)))
        except UnicodeDecodeError:
            out.append(-1)
    return out


def aliases_of(key, codec, written):
    """Every spelling that should resolve to this set, folded and de-duplicated."""
    found = {squeezed(written), squeezed(codec), squeezed(key)}
    for alias, target in encodings.aliases.aliases.items():
        if target == codec:
            found.add(squeezed(alias))
    for extra in EXTRA.get(key, []):
        found.add(squeezed(extra))
    return sorted(found)


def numbers(values, indent):
    """An array literal's elements, wrapped so no line passes the formatter's width.

    The comma goes on every element but the last: ziman refuses a trailing one before `]`."""
    width = 100 - len(indent)
    lines = []
    row = ""
    for i, v in enumerate(values):
        piece = str(v) + ("," if i + 1 < len(values) else "")
        if row == "":
            row = piece
        elif len(row) + 1 + len(piece) > width:
            lines.append(indent + row)
            row = piece
        else:
            row = row + " " + piece
    if row != "":
        lines.append(indent + row)
    return "\n".join(lines)


def main():
    seen_aliases = {}
    out = ['''// tables.zm -- the single-byte character sets, as data. GENERATED by tools/gen_charsets.py;
// `make charsets` rewrites it and `make charsets-check` fails when it is stale. Do not edit.
//
// **A single-byte set is a 128-entry table and nothing else**, so these are generated from
// Python's codec data -- CPython's vendored copy of the Unicode consortium's mappings -- rather
// than written out here. A table written from memory is wrong in a way no review catches, which
// is why `truetype` read Macintosh font names as Latin-1 for a day with a comment admitting it.
//
// Each set is three arrays: the 128 code points for 0x80 to 0xFF (-1 where the set has no
// character for that byte), and the same mapping the other way as a sorted list of code points
// with the byte for each, so encoding is a binary search rather than a scan of 128 entries.
//
// **Latin-1 and CP1252 are not here.** They are `Charset.Latin1` and `Charset.WinAnsi`, written
// by hand because one is arithmetic and the other is Latin-1 plus a 32-entry block; a table of
// either would be a second answer to a question that already has one.''',
           "// Produced by Python %d.%d.%d." % sys.version_info[:3], ""]

    constructors = []
    for key, codec, written, note in SETS:
        upper = table_of(codec)
        pairs = sorted((cp, 0x80 + i) for i, cp in enumerate(upper) if cp >= 0)
        out.append("// %s: %s." % (written, note))
        out.append("val %s_upper = [" % key)
        out.append(numbers(upper, "    "))
        out.append("]")
        out.append("")
        out.append("val %s_points = [" % key)
        out.append(numbers([cp for cp, _ in pairs], "    "))
        out.append("]")
        out.append("")
        out.append("val %s_bytes = [" % key)
        out.append(numbers([b for _, b in pairs], "    "))
        out.append("]")
        out.append("")
        out.append("// %s (%s)." % (written, note))
        out.append("pub fn %s() -> Charset {" % key)
        out.append("    return Charset.SingleByte(SingleByteSet {")
        out.append('        name: "%s",' % written)
        out.append("        upper: %s_upper," % key)
        out.append("        points: %s_points," % key)
        out.append("        bytes: %s_bytes" % key)
        out.append("    })")
        out.append("}")
        out.append("")
        for alias in aliases_of(key, codec, written):
            if alias in seen_aliases:
                raise SystemExit("alias %r is claimed by both %s and %s" % (alias, seen_aliases[alias], key))
            seen_aliases[alias] = key
            constructors.append((alias, key))

    out.append('''// The set a written name means, for the names this file carries.
//
// **Grouped by first character, which is worth 5x and is the reason this is not one flat chain.**
// 106 comparisons in a row cost 2.3 microseconds for the last name and the same for any name that
// is *not* here -- and a miss is the common case, because `charset_named` asks the hand-written
// sets first and reaches this only to find `utf8` is not in it. Measured at 200,000 lookups:
//
// | | flat | grouped |
// |---|---|---|
// | the last name | 457 ms | **151 ms** |
// | a name that is not here | 466 ms | **93 ms** |
//
// A second level inside each group, by length, was measured too and buys nothing (116 ms against
// 114): some bucket still holds nineteen names, so the split stops partitioning. A map is about
// twice as fast again at 50 ms and is not what this file is -- a table of names reads as a table.
//
// Each group returns rather than falling through, which is safe because the groups partition on
// the first character: a name beginning with `i` that is not in the `i` group is in none of them.
fn table_named(key: string) -> Option<Charset> {''')
    groups = collections.OrderedDict()
    for alias, key in sorted(constructors):
        groups.setdefault(alias[0], []).append((alias, key))
    for first, members in groups.items():
        out.append('    if starts_with(key, "%s") {' % first)
        for alias, key in members:
            out.append('        if key == "%s" {' % alias)
            out.append("            return Option<Charset>.Some(%s())" % key)
            out.append("        }")
        out.append("        return Option<Charset>.None")
        out.append("    }")
    out.append("    return Option<Charset>.None")
    out.append("}")
    out.append("")
    out.append('''// Every set this file carries, for a caller that wants to offer a list and for the round-trip
// test that holds all of them to the same promise.''')
    out.append("pub fn single_byte_sets() -> [Charset] {")
    out.append("    return [")
    out.append(numbers(["%s()" % key for key, _, _, _ in SETS], "        "))
    out.append("    ]")
    out.append("}")

    with open(OUT, "w") as f:
        f.write("\n".join(out).rstrip("\n") + "\n")
    print("gen_charsets.py: %d sets, %d aliases -> %s" % (len(SETS), len(constructors), os.path.relpath(OUT, ROOT)))


main()
