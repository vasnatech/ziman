# `net`

Blocking TCP: connect, listen/accept, read, write, close.
Blocking is deliberate. Ziman has real OS threads (`spawn`), so a
blocked read only blocks its own thread -- thread-per-connection is
the natural model here, with no event loop or async needed, and it is
exactly what the `pg` and `mysql` drivers want (blocking reads inside a wire protocol).

A `Socket`/`Listener` is an int handle to a native object. Close it explicitly -- and `defer`
makes that safe: write `val s = try net.connect(host, port)` then `defer net.close(s)`, and the
socket is closed on every way out of the block (a `try` that returns early, a `return`, falling off
the end). A socket you forget to close is not lost for good: once nothing references it, the
garbage collector closes its descriptor (and when the process runs out of descriptors it
collects at once and retries) -- but that is a safety net, not a plan, since a collection may be
far off; close promptly. Close a socket from the thread that
owns it: closing one another thread is blocked reading wakes that
read with an error, but the descriptor number is then free for the OS
to reuse.

Data is `[byte]` (see `number` for big-endian ints and `string_to_bytes` / `bytes_to_string`),
plus `read_text`/`write_text` for text protocols like HTTP. A clean
close by the peer is `Err(Closed)`, never an empty array, so an EOF
can't be silently forgotten -- the one place it is legitimate (a body
that ends when the connection does) matches on `Closed` explicitly.

TLS (client side) upgrades a connected socket *in place*: after
`start_tls`, the same `read`/`read_exact`/`write`/`read_text`/
`set_timeout`/`close` work over the encrypted connection, so nothing
built on `Socket` (the `pg` driver, a future HTTP layer) needs to know
which it has. Verification is ON by default -- the certificate chain
against the system CA store, and that it is valid for the host name
(or IP address) you connected to. Turn it down only deliberately, with
`start_tls_with`: `TlsVerify.Chain` skips the host-name check,
`TlsVerify.Off` skips verification entirely (encryption only, open to a
man in the middle). Minimum protocol is TLS 1.2. A TLS socket must not
be read and written from two threads at once without care -- each
operation is serialised by a lock, so it is safe but a blocked read
delays a write.

Server-side TLS is `listen_tls`. Not yet: client certificates, ALPN,
Unix-domain sockets, UDP, and a connect timeout beyond the OS default.

`use net` -- or `use net.Name` to bring one item in bare.

## Example

```
use net

// An echo server and a client in one program, over the loopback interface.
fn echo(l: net.Listener) {
    val s = unwrap(net.accept(l))
    val got = unwrap(net.read_text(s, 100))
    unwrap(net.write_text(s, concat("echo: ", got)))
    net.close(s)
}

fn sender(l: net.Listener) {
    val c = unwrap(net.accept(l))
    unwrap(net.write_text(c, "first\r\nsecond\nthird"))
    net.close(c)
}

fn main() {
    val listener = unwrap(net.listen("127.0.0.1", 0))
    spawn echo(listener)

    val s = unwrap(net.connect("127.0.0.1", net.listener_port(listener)))
    unwrap(net.set_timeout(s, 2000))
    unwrap(net.write_text(s, "hi"))
    print(unwrap(net.read_text(s, 100)))
    net.close(s)

    // Lines from a peer, read lazily until it closes.
    val again = unwrap(net.listen("127.0.0.1", 0))
    spawn sender(again)
    val t = unwrap(net.connect("127.0.0.1", net.listener_port(again)))
    for line in net.lines(t) {
        print(unwrap(line))
    }
    net.close(t)

    // A failure is a NetError saying what kind, and net_error_message turns it into text.
    val refused = net.connect("127.0.0.1", 1)
    print(is_ok(refused))
    print(net.net_error_message(unwrap_err(refused)))
}
```

Output:

```
echo: hi
first
second
third
false
connection refused: Connection refused
```

### `NetError`

```
pub enum NetError {
    ConnectionRefused(string)
    HostNotFound(string)
    Timeout(string)
    Closed(string)
    Io(string)
    Tls(string)
}
```

Why a network call failed; each carries the system's message. `Closed` is a peer that hung up,
`Timeout` a deadline set with `set_timeout`, `Tls` a handshake or certificate failure.

### `TlsVerify`

```
pub enum TlsVerify {
    Off
    Chain
    Full
}
```

How much of a server's certificate to check. `Full` (the default) is
the chain against the trusted CAs *and* that it is valid for the host;
`Chain` is the chain only; `Off` checks nothing.

### `Socket`

```
pub struct Socket {
    handle: int
}
```

A connected TCP (or TLS) socket. Opaque: use `read`, `write` and `close`. One that is never
closed is closed when it is garbage collected.

### `Listener`

```
pub struct Listener {
    handle: int
}
```

A listening TCP socket from `listen` or `listen_tls`. Opaque: use `accept`, `listener_port` and
`close`.

### `net_error_message`

```
pub fn net_error_message(e: NetError) -> string
```

The error as one line of text, for logging or showing to a user: `connection refused: ...`.

### `connect`

```
pub fn connect(host: string, port: int) -> Result<Socket, NetError>
```

`host` is a name or an address (IPv4 or IPv6).

### `connect_unix`

```
pub fn connect_unix(path: string) -> Result<Socket, NetError>
```

Connects to a Unix-domain socket, which is a filesystem path rather than a host and port -- a
local PostgreSQL or MySQL usually listens on one, and it is the usual way to talk to a service on
the same machine without going near the network stack. The `Socket` it answers behaves exactly
like a TCP one: `read`, `write`, `lines`, `set_timeout` and `close` all work unchanged, and so
does `start_tls`, though TLS over a local socket is rarely what you want.

### `listen_unix`

```
pub fn listen_unix(path: string) -> Result<Listener, NetError>
```

Listens on a Unix-domain socket path. Two things differ from `listen`, both reported rather than
hidden: the path has an operating-system limit of 107 bytes (much shorter than a file path's), and
binding fails if the path already exists -- including when it is a socket left behind by a process
that has gone. It is not removed for you, because a stale socket and a live one are
indistinguishable from here and removing the wrong one would take down a running server; the error
says so, and `fs.remove` is the answer once you know which it is. `listener_port` answers -1 for
one of these, there being no port to answer.

### `listen`

```
pub fn listen(host: string, port: int) -> Result<Listener, NetError>
```

Port 0 asks the OS for a free port; `listener_port` says which.

### `listen_tls`

```
pub fn listen_tls(host: string, port: int, cert_file: string, key_file: string) -> Result<Listener, NetError>
```

A listener whose accepted sockets are already TLS (server side, TLS 1.2 or later): `cert_file`
is a PEM certificate (or chain), `key_file` its PEM private key. `accept` completes the handshake
before returning the socket -- giving up on a client that stalls in it after 10 seconds -- and
reads, writes and `close` then work over the encrypted connection like any other `Socket`. A
failed handshake is an `Err(Tls)` from `accept`, so an accept loop just goes round again. An Err
from `listen_tls` itself means the certificate or key could not be used, or the port is not available.

### `listener_port`

```
pub fn listener_port(l: Listener) -> int
```

The port a listener is bound to (useful after `listen(host, 0)`, which picks a free one), or -1
for a Unix-domain listener, which has a path instead.

### `peer_address`

```
pub fn peer_address(s: Socket) -> Option<string>
```

The address at the other end of `s`, as text ("192.0.2.7" or "2001:db8::1"), with no port. `None`
when the socket is not connected any more or the address family is one this does not know -- there is
nothing to retry, so it is an absence rather than an error.

**This is the transport peer, and only that.** It is not `X-Forwarded-For`: that header is written by
whatever is in front, and a client that is *not* behind a proxy can put anything it likes in it. A
server that read it by default would let every caller choose its own identity, which is exactly
backwards for the thing an address is usually wanted for -- telling one client from another in order
to rate-limit. An application that sits behind a proxy it trusts has to say so itself, and read the
header itself, knowing how many hops it owns. This library will not guess at that.

### `accept`

```
pub fn accept(l: Listener) -> Result<Socket, NetError>
```

Blocks until a client connects.

### `read`

```
pub fn read(s: Socket, max: int) -> Result<[byte], NetError>
```

Blocks until at least one byte is available, then returns whatever
has arrived, up to `max` bytes.

### `read_exact`

```
pub fn read_exact(s: Socket, count: int) -> Result<[byte], NetError>
```

Exactly `count` bytes, or an error (`Closed` if the peer went away
first) -- the shape a length-prefixed protocol wants.

### `write`

```
pub fn write(s: Socket, data: [byte]) -> Result<int, NetError>
```

Writes every byte before returning, and reports how many.

### `read_text`

```
pub fn read_text(s: Socket, max: int) -> Result<string, NetError>
```

Text mode, for text protocols. A string cannot carry a 0 byte, so a
read that receives one is an error rather than a silent truncation.

### `write_text`

```
pub fn write_text(s: Socket, text: string) -> Result<int, NetError>
```

Writes the whole string (UTF-8) before returning, and reports how many bytes.

### `lines`

```
pub fn lines(s: Socket) -> Iter<Result<string, NetError>>
```

The lines a peer sends, read lazily as a `for` loop asks for them: `for line in net.lines(socket) { val
text = try line ... }`. Each item is a `Result<string, NetError>`: a line without its `\n` or `\r\n` (a last
line the peer ends the connection on counts), or an Err for a line with a NUL byte, one longer than 1 MiB,
or a read error (`Timeout`, `Closed` mid-line, ...); after a read error or an over-long line the
iteration ends. It ends normally when the peer closes the connection. It reads ahead in blocks of up to
4096 bytes, so take all of a socket's lines from one `lines` iterator, and use `read` or `read_exact`
on the socket only when you are not reading lines. The iterator does not close the socket; that stays
yours. Set a timeout with `set_timeout` so a silent peer cannot block the loop for ever.

### `lines_with`

```
pub fn lines_with(s: Socket, max_line_bytes: int) -> Iter<Result<string, NetError>>
```

`lines` with a chosen longest line, in bytes.

### `set_timeout`

```
pub fn set_timeout(s: Socket, millis: int) -> Result<unit, NetError>
```

Applies to reads and writes on this socket; a call that waits longer
fails with `Timeout`. Without one, a read on a dead peer waits forever.

### `start_tls`

```
pub fn start_tls(s: Socket, host: string) -> Result<unit, NetError>
```

Upgrades a connected socket to TLS, verifying the server's certificate
fully (chain against the system CA store, and that it names `host`).

### `start_tls_with`

```
pub fn start_tls_with(s: Socket, host: string, verify: TlsVerify, ca_file: Option<string>) -> Result<unit, NetError>
```

The same, choosing how much to verify and optionally a PEM file of
trusted CAs to use instead of the system store (e.g. a private CA, or a
self-signed test certificate).

### `connect_tls`

```
pub fn connect_tls(host: string, port: int) -> Result<Socket, NetError>
```

connect + start_tls. The socket is closed if the handshake fails, so a
failed call leaks nothing.

### `connect_tls_with`

```
pub fn connect_tls_with(
    host: string,
    port: int,
    verify: TlsVerify,
    ca_file: Option<string>
) -> Result<Socket, NetError>
```

`connect_tls` choosing how much of the certificate to verify and, optionally, a PEM file of
trusted CAs. The socket is closed if the handshake fails.

## Idle connections

### `close`

```
pub fn close(s: Socket)
```

Closes the socket (and, for TLS, sends close_notify first). Safe to call once; a socket used after
this gives a runtime error. Another thread blocked reading it is woken with `NetError.Closed` --
closing from a different thread than the reader is supported, not merely tolerated: the socket
holds a reference while a read is in flight, so the descriptor cannot be closed and its number
handed to something else underneath it (docs/reviews/tasks.md A31).

### `close_listener`

```
pub fn close_listener(l: Listener)
```

Stops listening and closes the listener's socket. A thread blocked in `accept` on it gets
`NetError.Closed` -- which is how a server shuts down, so it is supported rather than tolerated:
see `close` just above on what makes that safe.
