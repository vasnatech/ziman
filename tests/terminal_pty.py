#!/usr/bin/env python3
"""Drives lib/core/terminal.zm through a real pseudo-terminal.

**Why a pty and not a fixture.** The escape-sequence table is a pure function and
tests/stdlib/terminal_test.zm covers every row of it with no terminal attached -- that is decision
3.2 of docs/design/terminal.md paying off. But it covers none of decision 3.1, which is the one
that must not be got wrong: a program that leaves raw mode behind gives the user a shell with no
echo and no line editing, and they have to type `reset` blind. Proving the settings go back needs a
terminal to read them back from, which means a pty.

Each mode below is one way a program can end. The verdict is printed as a PASS/FAIL line in
tests/run_tests.sh's format; a system with no pty prints SKIP rather than failing.

Usage: terminal_pty.py <name> <keys|after_scope|crash|sigterm|screen_sigterm|screen_crash> <prog.zm>
"""
import os
import pty
import select
import signal
import struct
import sys
import termios
import time
import fcntl

LFLAG = 3
CC = 6

NAME, MODE, PROGRAM = sys.argv[1], sys.argv[2], sys.argv[3]
BIN = os.environ.get("BIN", "./ziman")

# The keys the `keys` mode feeds, and the names the program must print for them. The point of each
# one is in the comment; they are not a random sample.
KEY_SCRIPT = [
    (b"a", "a"),                        # an ordinary character
    (b"\x1b[A", "Up"),                  # a CSI arrow, arriving as one burst
    (b"\x1bOP", "F1"),                  # SS3: what xterm sends for F1
    (b"\x1b", "Escape"),                # **a lone Escape** -- resolved only by the escape timeout
    (b"\xc3\xa9", "é"),            # a two-byte character, read one byte at a time
    (b"\x03", "Ctrl-C"),                # in raw mode this is a byte, not a signal
    (b"\x1b[3~", "Delete"),             # the numbered family
    (b"q", "q"),                        # quits
]


def verdict(ok, detail=""):
    print(("PASS " if ok else "FAIL ") + NAME + (" (" + detail + ")" if detail and not ok else ""))
    sys.exit(0 if ok else 1)


# The alternate screen is not a termios bit, so it cannot be read back with `tcgetattr` the way raw
# mode can. What the master *can* see is the sequence itself: the C restore handler writes it to
# descriptor 1, which is the pty slave. So the assertion is on the bytes, which is a direct
# observation of the restore rather than an inference from it.
LEAVE_SCREEN = b"\x1b[?1049l"
ENTER_SCREEN = b"\x1b[?1049h"


def mode_flags(attrs):
    echo = "echo" if attrs[LFLAG] & termios.ECHO else "no-echo"
    canon = "canon" if attrs[LFLAG] & termios.ICANON else "raw"
    return echo + "/" + canon


def same_settings(a, b):
    """Everything but the speeds, which a pty reports inconsistently across systems."""
    return a[0:4] == b[0:4] and a[CC] == b[CC]


try:
    pid, fd = pty.fork()
except OSError as e:
    print("SKIP " + NAME + " (no pty available: " + str(e) + ")")
    sys.exit(0)

if pid == 0:
    os.execv(BIN, [BIN, PROGRAM])

# A known window size, so `terminal.size()` has a number to be right about.
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
before = termios.tcgetattr(fd)

out = bytearray()
at_mark = None


def pump(seconds):
    """Reads whatever the program has written, and samples the terminal at the MARK."""
    global at_mark
    end = time.time() + seconds
    while time.time() < end:
        readable, _, _ = select.select([fd], [], [], 0.05)
        if not readable:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            return
        if not chunk:
            return
        out.extend(chunk)
        if at_mark is None and b"MARK" in out:
            # **While the program is still running**: a restore that only happened at exit fails here.
            at_mark = termios.tcgetattr(fd)


pump(0.6)
while_running = termios.tcgetattr(fd)

if MODE == "keys":
    for keystrokes, _ in KEY_SCRIPT:
        time.sleep(0.09)
        os.write(fd, keystrokes)
        pump(0.12)
    pump(1.0)
elif MODE in ("sigterm", "screen_sigterm"):
    # atexit does NOT run for a signal, so this is the case the C half's signal handler exists for.
    os.kill(pid, signal.SIGTERM)
    pump(0.8)
else:
    pump(1.6)

try:
    _, status = os.waitpid(pid, 0)
except ChildProcessError:
    status = 0
pump(0.3)
after = termios.tcgetattr(fd)
text = out.decode("utf-8", "replace").replace("\r\n", "\n")

if MODE == "keys":
    if "size 24x80" not in text:
        verdict(False, "the window size was not reported as 24x80: " + text.strip().replace("\n", " | "))
    if while_running[LFLAG] & termios.ECHO or while_running[LFLAG] & termios.ICANON:
        verdict(False, "raw mode was not actually entered (" + mode_flags(while_running) + ")")
    for _, expected in KEY_SCRIPT:
        if ("key " + expected) not in text:
            verdict(False, "did not report " + repr(expected) + ": " + text.strip().replace("\n", " | "))
    if "done" not in text:
        verdict(False, "did not finish cleanly: " + text.strip().replace("\n", " | "))
    if not same_settings(after, before):
        verdict(False, "the terminal was not restored (" + mode_flags(after) + ")")
    verdict(True)

if MODE == "after_scope":
    if at_mark is None:
        verdict(False, "the program never printed MARK: " + text.strip().replace("\n", " | "))
    if not same_settings(at_mark, before):
        verdict(False, "still in raw mode after the scope ended (" + mode_flags(at_mark) + ")")
    verdict(True)

if MODE == "crash":
    if "in raw mode" not in text:
        verdict(False, "never entered raw mode: " + text.strip().replace("\n", " | "))
    if "out of bounds" not in text:
        verdict(False, "did not die of the error it was written to die of")
    if not same_settings(after, before):
        verdict(False, "the terminal was not restored after a fatal error (" + mode_flags(after) + ")")
    verdict(True)

if MODE == "sigterm":
    if not same_settings(after, before):
        verdict(False, "the terminal was not restored after SIGTERM (" + mode_flags(after) + ")")
    if not (os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGTERM):
        verdict(False, "did not die of SIGTERM (status " + str(status) + ")")
    verdict(True)

if MODE in ("screen_sigterm", "screen_crash"):
    raw = bytes(out)
    if ENTER_SCREEN not in raw:
        verdict(False, "never entered the alternate screen: " + repr(raw[:200]))
    if b"READY" not in raw:
        verdict(False, "never finished painting: " + repr(raw[:200]))
    # **After the enter, not merely present**: a leave that arrived before the enter would be a
    # different program's, and a check that only looked for the sequence anywhere would pass on one.
    if raw.rfind(LEAVE_SCREEN) < raw.find(ENTER_SCREEN):
        verdict(False, "the alternate screen was never left: " + repr(raw[-200:]))
    if not same_settings(after, before):
        verdict(False, "the terminal settings were not restored (" + mode_flags(after) + ")")
    if MODE == "screen_sigterm":
        if not (os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGTERM):
            verdict(False, "did not die of SIGTERM (status " + str(status) + ")")
    else:
        if b"out of bounds" not in raw:
            verdict(False, "did not die of the error it was written to die of")
    verdict(True)

verdict(False, "unknown mode " + MODE)
