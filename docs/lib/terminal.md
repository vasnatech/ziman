# `terminal`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

Raw mode, one key at a time, and putting back what we changed.

**The gap this closes was found by an application, not by a wishlist.** A Ziman program could
print anywhere on the screen and could not learn that a key had been pressed, so every
interactive tool in this tree was either a line of text at a time or a browser.

**Two halves, and the split is the point** (docs/design/terminal.md decision 3.2). C reads bytes
and restores `termios`; everything about *keys* -- the escape-sequence table, the ESC ambiguity,
UTF-8 -- is here, because that is the half most likely to grow as terminals disagree and the half
that can be tested from a byte array with no terminal attached. `decode_key` is a pure function
over `[byte]`, and `tests/stdlib/terminal_test.zm` is the whole table.

**The terminal is restored however the program ends** (decision 3.1), which is why `with_raw` is
the public shape and not a pair of calls: `atexit` in the C half covers a program that *ends* in
raw mode, and nothing but a scope covers a program that keeps running after an early `return`.
A program that dies in raw mode leaves the user's shell with no echo and no line editing, and
they have to type `reset` blind -- worse than the crash that caused it.

**Linux and macOS** (decision 3.3). `termios` is POSIX, so the two come together; Windows needs a
different API entirely and says so rather than pretending.

What this is deliberately not: a widget toolkit, a mouse reader, a terminfo lookup, or colour
helpers. docs/design/terminal.md §8 has each one and why.

`use terminal` -- or `use terminal.Name` to bring one item in bare.

## Example

```
use terminal

// **Ask `is_tty` first.** A TUI whose standard input is a pipe should refuse rather than render a
// frame into a file, and everything else in this module is undefined when it is false. Under this
// manual's own test runner there is no terminal, which is why the rest of this example demonstrates
// the half that needs none.
fn interactive() -> Result<unit, string> {
    if !terminal.is_tty() {
        print("standard input is not a terminal")
        return ok(unit)
    }
    // **`with_raw` is the only way into raw mode**, and it puts the settings back whatever the body
    // does -- answers, fails, or dies. A `set_raw(true)`/`set_raw(false)` pair would leave the
    // user's shell with no echo on any early return, and they would have to type `reset` blind.
    return terminal.with_raw<unit>(fn() -> Result<unit, string> {
        val screen = try terminal.size()
        print(prelude.format("{} rows by {} columns", screen.rows, screen.cols))
        // The timeout is what makes a render loop possible: it is how a redraw happens when nothing
        // was typed, which is also how a resize is noticed (a resize is polled, not delivered).
        switch try terminal.read_key(50) {
            Some(key) { print(concat("you pressed ", terminal.key_name(key))) }
            None { print("nothing was typed") }
        }
        return ok(unit)
    })
}

// **The escape-sequence table is a pure function over bytes**, which is the whole reason it is in
// Ziman rather than in C: it can be read, tested and extended with no terminal attached.
fn decoding() {
    // One byte: a character, or a key with a name of its own. Enter is both `\r` and `\n`, because
    // raw mode turns off the translation between them.
    print(terminal.key_name(terminal.decode_key([97b])))
    print(terminal.key_name(terminal.decode_key([13b])))
    print(terminal.key_name(terminal.decode_key([3b])))

    // An escape sequence: `ESC [ A` is Up, and `ESC O P` is F1 -- two families, because xterm and
    // the vt220 disagreed and both survived.
    print(terminal.key_name(terminal.decode_key([27b, 91b, 65b])))
    print(terminal.key_name(terminal.decode_key([27b, 79b, 80b])))

    // A multi-byte character is one key.
    print(terminal.key_name(terminal.decode_key([195b, 169b])))

    // **A sequence the table does not know keeps its bytes.** That is not a fallback that should
    // never happen: terminals disagree, and a program that logs the bytes is how the gap gets
    // closed. Here it is Ctrl-Up, which `Key` has no way to say -- answering plain `Up` would fire
    // a shortcut on a key the user did not press.
    print(terminal.key_name(terminal.decode_key([27b, 91b, 49b, 59b, 53b, 65b])))
}

fn main() {
    decoding()
    switch interactive() {
        Ok(_) {}
        Err(why) { print(concat("terminal: ", why)) }
    }
}
```

Output:

```
a
Enter
Ctrl-C
Up
F1
é
unknown key [27 91 49 59 53 65]
standard input is not a terminal
```

### `Key`

```
pub enum Key {
    Char(string)
    Enter
    Tab
    Backspace
    Escape
    Delete
    Up
    Down
    Left
    Right
    Home
    End
    PageUp
    PageDown
    Insert
    Function(int)
    Ctrl(string)
    Unknown([byte])
}
```

A key press. **`Char` is one character and may be several bytes**; `Ctrl` is `"a"` to `"z"`.

**`Unknown` carries the bytes rather than dropping them**, for the reason a `Json` unknown-tag
error names the tag: a key this table has not met is a gap in the table, and a program that logs
the bytes can have it fixed. Silently swallowing input is how a terminal feature becomes
impossible to diagnose from the outside -- and with terminals disagreeing as much as they do,
being diagnosable from the outside is the only way the table ever gets finished.

### `Size`

```
pub struct Size {
    rows: int
    cols: int
}
```

The window, in character cells.

## The native half

### `is_tty`

```
pub native fn is_tty() -> bool
```

Whether standard input is a terminal. **Ask this first**: a TUI whose input is a pipe should
refuse rather than render a frame into a file, and everything else in this module is undefined
when it is false.

## The window

### `size`

```
pub fn size() -> Result<Size, string>
```

The terminal's size right now. **Call it every frame rather than caching it**: decision 3.4 makes
a resize polled rather than delivered, so this answering a new number is how a program learns the
window changed. `os.install_shutdown_handler()` sets the same precedent for a signal -- the
handler sets a flag and the program asks at a moment of its choosing.

## Raw mode as a scope

### `with_raw`

```
pub fn with_raw<T>(body: fn() -> Result<T, string>) -> Result<T, string>
```

Runs `body` with the terminal in raw mode, and **puts the settings back whatever `body` does** --
answers `Ok`, answers `Err`, or fails fatally (the C half's `atexit` covers the last one).

This is the whole of decision 3.5. Ziman has no `defer` and no destructors, so the alternative
was a `set_raw(true)`/`set_raw(false)` pair, whose failure mode is the one thing decision 3.1
says must not happen: an early `return` between the two leaves the user's shell broken, and no
amount of documentation prevents one. A closure cannot leak the mode. The cost is that `body`
cannot `return` out of the enclosing function, which in a render loop is no cost at all.

**A failed restore is reported only when `body` succeeded.** If both went wrong, `body`'s error
is the one the caller needs -- a restore failure is a consequence, and the `atexit` handler will
try again at exit anyway.

## Reading a key

### `read_key`

```
pub fn read_key(timeout_ms: int) -> Result<Option<Key>, string>
```

The next key press, or `None` if `timeout_ms` passed with nothing to report.

**No buffer and no reader object**: the native answers one byte, so every byte this reads was
asked for, and there is never a tail of a burst to keep somewhere. "Somewhere" would be
module-level mutable state in a module that otherwise has none.

The timeout applies to the *first* byte only. Once a key has started arriving the rest of it is
already in the driver's buffer, so the follow-up reads use `escape_timeout_ms` and in practice
return at once.

## The table, which is pure

### `decode_key`

```
pub fn decode_key(bytes: [byte]) -> Key
```

**The table.** Pure, total, and the reason decision 3.2 put this half in Ziman: it takes bytes and
answers a `Key`, so the whole of it is testable with no terminal attached.

Anything it does not recognise is `Unknown` carrying the bytes. That is not a fallback that
should never happen -- terminals disagree, and a program that logs the bytes is how the gap gets
closed.

## Naming a key

### `key_name`

```
pub fn key_name(key: Key) -> string
```

What to show a human: `"a"`, `"Up"`, `"Ctrl-C"`, `"F5"`, or the bytes of a key this table does not
know. The counterpart of `io.io_error_message` -- every module here has one, and a help line or a
key-binding list is the reason.
