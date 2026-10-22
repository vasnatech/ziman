#!/usr/bin/env python3
"""Generates the library manual, docs/lib/, from the bundled modules' own source.

Each module under lib/ becomes docs/lib/<name>.md: its header comment is the introduction, every
`pub` item is an entry (its signature, then its own comment), and a `// ---- title` divider line in
the source starts a section. A module's worked example, when there is one, is
docs/lib/examples/<name>_example.zm (a program the test suite runs); it is included in the page.
docs/lib/README.md is the index. Nothing here is written by hand except the example programs and
the grouping in GROUPS below, so the manual cannot drift from the code: regenerate it with
`make corelib-reference`; `make lib-manual-check` fails if the committed pages differ.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "lib")

GROUPS = [
    ("Data and formats", "core", ["json", "ndjson", "csv", "toml", "yaml", "xml", "markdown", "properties",
                                   "number", "math", "bits", "uuid", "regex", "compress", "crypto", "jwt", "otp", "mime"]),
    # The group's directory is where the source is read from, so a module listed here lives in
    # lib/<dir>/. `schedule` sits under Time because it is built on `time` and `clock` and reads
    # as part of that family; `clock` stays under System with `random` and `uuid`, the other thin
    # wrappers over a native primitive.
    ("Time", "time", ["time", "zone", "schedule"]),
    ("System", "core", ["fs", "os", "process", "thread", "log", "metrics", "random", "clock"]),
    ("Networking", "core", ["net", "url"]),
    ("HTTP", "http", ["http", "server", "client"]),
    ("Databases", "db", ["db", "datasource", "migrate", "sqlite", "pg", "mysql"]),
    ("Testing", "core", ["test"]),
    ("Graphics", "graphics", ["geom", "shape", "paint", "scene", "pdfobj", "pdf", "truetype", "font", "image", "stroker", "raster", "layout"]),
]


def read_lines(path):
    with open(path) as f:
        return f.read().split("\n")


def comment_text(lines):
    """The text of a run of `//` lines, one string per line, with the marker and one space removed."""
    out = []
    for l in lines:
        t = l[2:]
        if t.startswith(" "):
            t = t[1:]
        out.append(t)
    return out


def render_comment(text_lines):
    """Comment lines as markdown: an indented run that follows a blank line becomes a code block,
    other lines pass through."""
    out = []
    i = 0
    while i < len(text_lines):
        l = text_lines[i]
        after_blank = i == 0 or not text_lines[i - 1].strip()
        if l.startswith("  ") and l.strip() and after_blank:
            block = []
            while i < len(text_lines) and (text_lines[i].startswith("  ") or not text_lines[i].strip()):
                block.append(text_lines[i] if text_lines[i].strip() else "")
                i += 1
            indent = min(len(b) - len(b.lstrip()) for b in block if b)
            block = [b[indent:] for b in block]
            while block and not block[-1]:
                block.pop()
            out.append("")
            out.append("```")
            out.extend(block)
            out.append("```")
            out.append("")
        else:
            out.append(l)
            i += 1
    return "\n".join(out).strip("\n")


def signature(lines, i):
    """The declaration starting at lines[i] and the index after it (a struct or enum runs to its
    closing brace, a function to the end of its signature)."""
    first = lines[i]
    if re.match(r"(pub )?(struct|enum) ", first):
        j = i
        while lines[j] != "}":
            j += 1
        return "\n".join(lines[i : j + 1]), j + 1
    sig = first
    j = i
    while sig.count("(") > sig.count(")"):
        j += 1
        sig += "\n" + lines[j]
    sig = re.sub(r" \{$", "", sig)
    if re.search(r"\{\s*$", sig.split("\n")[-1]):
        sig = re.sub(r"\s*\{\s*$", "", sig)
    return sig, j + 1


def prelude_parts():
    """lib/prelude/<part>.zm, in the Makefile's PRELUDE_PARTS order.

    Read from the Makefile rather than listed here, so the manual cannot come to disagree with
    what is actually built -- the same reason src/lsp.c's completion list and the VS Code grammar
    are generated. A part missing from PRELUDE_PARTS is not in the binary, so it must not be in
    the manual either."""
    mk = read_lines(os.path.join(ROOT, "Makefile"))
    for line in mk:
        if line.startswith("PRELUDE_PARTS :="):
            names = line.split(":=", 1)[1].split()
            paths = [os.path.join(ROOT, "lib", "prelude", n + ".zm") for n in names]
            missing = [p for p in paths if not os.path.exists(p)]
            if missing:
                sys.exit("gen_lib_manual.py: PRELUDE_PARTS names a file that is not there: %s"
                         % ", ".join(missing))
            return paths
    sys.exit("gen_lib_manual.py: no PRELUDE_PARTS line in the Makefile -- it was renamed, and "
             "this tool reads it rather than keeping its own copy")


def parse(path):
    # A list of paths is the prelude, which is several files that make one page: each part becomes
    # a section, and only the first part's header describes the page (every part repeats the same
    # "merged bare into every program" paragraph, which would otherwise be printed ten times).
    paths = path if isinstance(path, list) else [path]
    # `builtin fn` belongs here for the same reason it exists: a builtin with no documented
    # signature is one a reader cannot find. See DESIGN.md's "`native` and builtin are two
    # different mechanisms".
    # `pub` is required of a prelude declaration too since 2026-10-22: a non-`pub` one cannot be
    # called from outside the prelude, so listing it in the manual would advertise as vocabulary
    # something a reader's program is refused for using. It was optional here while natives were
    # advertised whatever their visibility said -- 143 of them carried no modifier -- and that
    # sweep is what made one rule serve both.
    decl = r"pub (native fn|builtin fn|fn|struct|enum|val) "
    header = []
    entries = []  # (section, comment lines, signature)
    for part_index, part_path in enumerate(paths):
        hdr, part_entries = parse_one(part_path, decl,
                                      os.path.basename(part_path) if len(paths) > 1 else None)
        if part_index == 0:
            header = hdr
        entries += part_entries
    return header, entries


def parse_one(path, decl, fixed_section):
    lines = read_lines(path)
    i = 0
    header = []
    while i < len(lines) and lines[i].startswith("//"):
        header.append(lines[i])
        i += 1
    entries = []  # (section, comment lines, signature)
    section = fixed_section
    pending = []
    while i < len(lines):
        l = lines[i]
        if l.startswith("//"):
            m = re.match(r"// -{3,}\s*(.*?)\s*-*$", l)
            if m and (m.group(1) or l.strip("/ -") == ""):
                if m.group(1) and fixed_section is None:
                    section = m.group(1)
                pending = []
            else:
                pending.append(l)
            i += 1
            continue
        if l.startswith("#["):
            # An attribute belongs to the declaration after it, so it must not come between a doc
            # comment and the thing that comment describes: `pending` is carried across it rather
            # than cleared. Without this, every documented declaration carrying one loses its whole
            # description from the manual -- `thread.zm`'s `#[thread_key] ThreadLocal<T>` and
            # `server.zm`'s `#[deprecated] open_session_with_timeout` are both written that way.
            i += 1
            continue
        if re.match(decl, l):
            sig, i = signature(lines, i)
            entries.append((section, pending, sig))
            pending = []
            continue
        pending = []
        i += 1
    return header, entries


def first_sentence(text):
    t = " ".join(x.strip() for x in text if x.strip())
    m = re.match(r"(.+?[.!?])(\s|$)", t)
    return (m.group(1) if m else t).strip()


def intro(header, name):
    text = comment_text(header)
    if text:
        first = re.sub(r"^%s\.zm\s*--\s*" % re.escape(name), "", text[0])
        text[0] = first[:1].upper() + first[1:]
    return text


def is_experimental(lines):
    """Whether the module carries `#![experimental]` -- the module-level attribute is the single
    place that claim lives, so this manual and tools/gen_corelib_reference.sh both read it from the
    source instead of each repeating it by hand (docs/ROADMAP.md)."""
    return any(l.strip() == "#![experimental]" for l in lines)


def summary_of(text):
    """One line for the index: the first sentence, cut at its first colon, dash or parenthesis."""
    s = first_sentence(text)
    m = re.search(r"(:| -- | \(|;)", s)
    if m and m.start() >= 30:
        s = s[: m.start()]
    return s.rstrip(",;: ").rstrip(".") + "."


def page(name, path, kind):
    header, entries = parse(path)
    text = intro(header, name)
    out = ["# `%s`" % name, ""]
    with open(path[0] if isinstance(path, list) else path) as f:
        experimental = is_experimental(f.read().split("\n"))
    if experimental:
        out += ["> **Experimental** -- see docs/DESIGN.md's \"Experimental modules\" entry. Generated from this",
                "> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.",
                ""]
    body = render_comment(text)
    if body:
        out += [body, ""]
    if name == "prelude":
        out += ["*This module is never imported: everything it declares is available bare in every program.*", ""]
    else:
        out += ["`use %s` -- or `use %s.Name` to bring one item in bare." % (name, name), ""]
    example = os.path.join(OUT, "examples", name + "_example.zm")
    if os.path.exists(example):
        with open(example) as f:
            out += ["## Example", "", "```", f.read().rstrip("\n"), "```", ""]
        expected = example[:-3] + ".out"
        if os.path.exists(expected):
            with open(expected) as f:
                out += ["Output:", "", "```", f.read().rstrip("\n"), "```", ""]
    current = object()
    last_name = None
    for section, comment, sig in entries:
        if section != current:
            current = section
            last_name = None
            if section:
                # A prelude part is a file name, so it reads as one rather than being
                # sentence-cased into "Prelude.zm".
                title = ("`%s`" % section if section.endswith(".zm")
                         else section[:1].upper() + section[1:])
                out += ["## %s" % title, ""]
        name_m = re.match(r"(?:pub )?(?:native fn|builtin fn|fn|struct|enum|val) (\w+)", sig)
        # `decl_name`, not `name`: `name` is this function's own parameter, the *module's* name,
        # and the last line uses it to strip the `<module>.zm --` prefix off the header comment.
        # Shadowing it here silently made every index summary read "Json.zm -- JSON as a ..."
        # instead of "JSON as a ...", because the prefix was then stripped with the last
        # declaration's name. Python has no -Wshadow; the generated README did.
        decl_name = name_m.group(1)
        # **One heading per name, however many signatures it has.** A `builtin fn` overload set is
        # several declarations under one name -- `len` has nine -- and a heading each would be nine
        # identical `### len` entries, which is worse than useless in a page with an anchor per
        # heading. Consecutive declarations of one name share its heading and stack their
        # signatures, which is also how a reader wants to see "what does len accept".
        if decl_name != last_name:
            out += ["### `%s`" % decl_name, ""]
            last_name = decl_name
        out += ["```", sig, "```", ""]
        c = render_comment(comment_text(comment))
        out += [c, ""] if c else []
    return "\n".join(out).rstrip("\n") + "\n", summary_of(intro(header, name))


def main():
    os.makedirs(OUT, exist_ok=True)
    index = ["# Bundled library manual", "",
             "One page per module. Every page is generated from the module's own source -- its header",
             "comment is the introduction and each `pub` item's comment is its entry -- so it is kept",
             "true by `make corelib-reference`. For the language itself see [SYNTAX.md](../SYNTAX.md); for",
             "a bare signature list see [CORELIB.md](../CORELIB.md), and for which module uses which see",
             "[dependencies.md](dependencies.md).", "",
             "Any program can `use` these with no local file. Names are flat: a module is called by its file",
             "name (`use db`, `use pg`), whatever folder it lives in. A file of your own with the same name",
             "wins over the bundled one.", "",
             "Everything that can fail returns a `Result`, and an absent value is an `Option`; nothing here",
             "throws. See [SYNTAX.md](../SYNTAX.md) for `?`, `Result` and `Option`.", ""]
    seen = set()
    for title, folder, names in GROUPS:
        index += ["## %s" % title, "", "| Module | What it is |", "|---|---|"]
        for name in names:
            path = os.path.join(ROOT, "lib", folder, name + ".zm")
            text, summary = page(name, path, folder)
            with open(os.path.join(OUT, name + ".md"), "w") as f:
                f.write(text)
            index.append("| [`%s`](%s.md) | %s |" % (name, name, summary.replace("|", "\\|")))
            seen.add(path)
        index.append("")
    path = prelude_parts()
    text, summary = page("prelude", path, "")
    with open(os.path.join(OUT, "prelude.md"), "w") as f:
        f.write(text)
    index += ["## Always available", "", "| Module | What it is |", "|---|---|",
              "| [`prelude`](prelude.md) | %s |" % summary.replace("|", "\\|"), ""]
    seen.update(path)
    for folder in ("core", "db", "http", "time"):
        for fn in sorted(os.listdir(os.path.join(ROOT, "lib", folder))):
            p = os.path.join(ROOT, "lib", folder, fn)
            if fn.endswith(".zm") and p not in seen:
                sys.exit("gen_lib_manual.py: %s is in no group; add it to GROUPS" % p)
    wanted = {n + ".md" for _, _, names in GROUPS for n in names} | {"prelude.md", "README.md", "dependencies.md"}
    for stale in sorted(os.listdir(OUT)):
        if stale.endswith(".md") and stale not in wanted and stale != "library-map.md":
            os.remove(os.path.join(OUT, stale))
    with open(os.path.join(OUT, "README.md"), "w") as f:
        f.write("\n".join(index).rstrip("\n") + "\n")


main()
