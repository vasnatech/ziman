# Design journal, part 8 (entries dated 2026-10-25 to 2026-10-26)

Part of the [design journal](README.md) -- see its "How to read this journal" for what a dated entry means.

## `xml`: six namespace MUST NOTs, and a comment the writer refused but the reader did not (2026-10-25)

`xml.zm` reads XML 1.0 well. Of 31 valid documents -- mixed content, CDATA, every entity form, numeric references in both bases, default and prefixed namespaces, a BOM, CRLF, a 500-deep tree -- it parsed and wrote back all 31, with the details right: CRLF normalised to LF, a newline in an attribute written as `&#10;`, `]]>` in content refused. Its well-formedness checking is thorough too: mismatched tags, two roots, text outside the root, unquoted and duplicated attributes, bare `&`, undefined entities, a NUL or surrogate character reference, `--` inside a comment, a PI named `xml` after the start, names beginning with a digit, undeclared prefixes, the depth limit, a declared non-UTF-8 encoding, and DOCTYPE in any form all come back as errors with a line number.

**What it did not check was the namespace specification's reserved names.** "Namespaces in XML" fences off two prefixes and two namespace names with six separate MUST NOTs, and all six documents parsed:

* `xmlns:xmlns="urn:x"` -- "The prefix `xmlns` ... *MUST NOT* be declared."
* `<xmlns:a/>` -- "Element names *MUST NOT* have the prefix `xmlns`."
* `xmlns="http://www.w3.org/2000/xmlns/"` and `xmlns:p="http://www.w3.org/2000/xmlns/"` -- of that namespace name, "Other prefixes *MUST NOT* be bound to this namespace name, and it *MUST NOT* be declared as the default namespace."
* `xmlns:xml="urn:wrong"` -- the prefix `xml` "*MAY*, but need not, be declared, and *MUST NOT* be bound to any other namespace name."
* `xmlns:p="http://www.w3.org/XML/1998/namespace"` and `xmlns="http://www.w3.org/XML/1998/namespace"` -- its namespace name carries the same sentence as the one above.
* `xmlns:p=""` -- for a prefixed declaration "the attribute value *MUST NOT* be empty", while a default declaration "*MAY* be empty". The two cases cannot share a check, which is presumably why one of them was missing: `xmlns=""` is how an element goes back into no namespace and is correct, so the obvious shared guard would have broken a legal document.

A tree where `xmlns` is itself a namespace prefix, or where `xml:lang` has been rebound away from the language namespace, is one nobody can mean. `check_declarations` now refuses all of them, and the fixtures keep the legal neighbours beside each -- `xmlns:xml` bound to exactly its own name, `xmlns=""`, an element merely *named* `xmlns` (only the prefix is reserved).

**The sharper bug: `<!-- a --->` parsed, and then could not be written.** `Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'`, so the content cannot end with a dash. The *writer* knew this -- it answered "a comment cannot end with '-'" -- and the reader did not, so a well-formedness error turned into a tree that round-trips to an `Err`. **A reader that can produce a tree its own writer refuses is the worst shape this kind of bug takes**: the document is accepted, nothing complains, and the failure arrives later at whatever tries to serialise it. The reader refuses it now, and a new test walks thirteen documents through parse-write-parse-write and asserts the two texts match, which is the invariant that was actually broken rather than the one case that revealed it.

**One message named an attribute that appears once.** `<a xmlns:p="urn:x" xmlns:q="urn:x" p:id="1" q:id="2"/>` violates "no element have two attributes with the same expanded name", and that *was* detected -- but reported as "attribute 'q:id' appears twice", and `q:id` appears exactly once. The collision is between two differently written names that resolve to one, which is the part a reader cannot see for themselves, so it now says which two and what they share. A plain repeat still reports as a plain repeat.

**A correction to my own finding, which is the part worth keeping.** I reported that a NUL byte in text was accepted and silently dropped -- `<a>\0</a>` coming back as `<a/>`, against the module's own header promising control characters are "an error on both sides rather than something silently dropped". That was wrong, and the fault was in my probe. `append_byte` returns a `bool`, returns `false` for a zero byte, and appends nothing: a Ziman `string` cannot hold a NUL at all (`bytes_to_string` says so outright -- "bytes contain a NUL byte and cannot be a string"), which is tied to `value_cstr` handing `ptr` straight to libc. My probe ignored that return, so the document I thought I was parsing was `<a></a>`, and `xml` parsed it correctly. **A builder whose result is a `bool` makes a probe that discards it test a different input than the one it prints**, and the reachable controls -- 1 through 8, 11, 12, 14 through 31 -- were refused all along, as was `&#0;` on the character-reference path.


## `yaml`: four values read wrong, and a newline that was never a line (2026-10-25)

`yaml.zm` is a deliberately strict YAML 1.2 subset, and the strictness holds up. Its scalar schema is exactly the 1.2 core schema, checked value by value: `0b1010` is a string (the core schema has no binary), `.5` and `5.` are floats (it does allow those), `1_000` and `1:30` are strings (1.1 features, not 1.2), `007` is a string, `yes`/`no`/`on`/`off` are strings, and `Null`/`NULL`/`True`/`FALSE` are handled. Everything the header says it refuses -- anchors, aliases, tags, complex keys, multiple documents, tab indentation, block-scalar indentation indicators, `.inf`/`.nan`, directives -- it refuses with a line number. Duplicate keys are caught in both block and flow style.

**Four things it read, and read wrong.** Each is "misread rather than reported", which is the one failure the header promises not to have:

* **Keep chomping added a newline.** The spec's Example 8.4 wants `keep: |+` over `  text` to be `"text\n"`; this gave `"text\n\n"`. Example 8.5, with a trailing empty line, wants `"# text\n\n"`; this gave three.
* **A folded scalar folded around more-indented lines.** `>` over `one` / `  deep` / `two` gave `one   deep two` -- three lines run together with the indentation stranded in the middle -- where §6.5 says "folding does not apply to line breaks surrounding text lines that contain leading white space". The giveaway was next door: `|` over the same body already produced `one\n  deep\ntwo`, so the module disagreed with itself.
* **`...` was skipped.** `a: 1` / `...` / `b: 2` came back as `{"a":1,"b":2}` -- one mapping assembled from two documents, by a module whose header lists "multiple documents" as unsupported. It hid because `...` followed by `---` *was* caught, by the `---` check.
* **The `<<` merge key was refused in block style and read as data in flow style.** `{<<: {x: 1}}` became a mapping with a key named `<<`.

**The chomping bug was not in the chomping.** `tokenize` splits on `"\n"`, and `split` yields a final empty element for text that ends with a newline -- so `"a: 1\n"` arrived as two lines, the second empty. Nothing cared: blank lines are skipped everywhere, clip and strip discard trailing blanks by definition. Keep is the one place that *counts* them, so the file's own terminating newline became a trailing empty line and keep added one more `\n` than it should. The fix is one condition in the tokenizer rather than an adjustment inside `block_scalar`, because the phantom line was wrong everywhere and only visible in one place; compensating at the visible end would have left the next reader of `lines` the same trap. **A bug that shows up in one function is not evidence that one function is where it lives.**

The other three were each a single missing branch, and all three were missing in a *second* path rather than the first: folding handled literal blocks, blank lines and ordinary folds but not the more-indented case; the merge key was checked where block mappings build keys but not where flow mappings do; `...` had a branch that matched it and then did nothing. The pattern across all three is a check written once for the path in front of the author and not carried to the parallel one -- the same shape as `xml`'s comment rule, which the writer enforced and the reader did not.

**A divergence I found, measured, and left.** A block scalar at the very end of a document with no final newline comes out with one: `a: |` over `  one` gives `"one\n"`, where the spec's `b-chomped-last` lets end-of-file stand in for the final break and would give `"one"`. Fixing it means telling "ended with a newline" from "ended at EOF" after the tokenizer has deliberately stopped distinguishing them -- a flag threaded through for a file that does not end in a newline and whose last construct is a block scalar. The input is treated as though it ends with a line break, clip and keep agree with each other, and that is written down here rather than discovered later.

Verification worth naming: beyond the four corners, a round-trip pass puts twenty documents through `parse` → `format` → `parse` and compares the trees, including every line-ending shape the tokenizer change could have disturbed -- no trailing newline, one, two, CRLF throughout, leading blanks, a document of only newlines, and a block scalar as the last thing in the file. `yaml_basic.out`, which exercises a folded block with a blank line and a literal block with an indented line, is byte-identical after the change.


## `db`'s URL parser: an IPv6 host nobody could reach, and a parameter dropped in silence (2026-10-25)

`db.parse_url` is the one function every driver reaches a server through, and most of it is right in the places that are easy to get wrong. The "LAST `@` separates userinfo from host" rule works, so a literal `@` in a password parses; `%40`, `%3A`, `%2F` and multi-byte `%C3%A9` all decode; `percent_decode` refuses a truncated escape, bad hex, and a `%00` that would put a NUL in a string. Four things were wrong, and three of the four shared a shape: the parser accepted something and let the *consequence* surface somewhere else.

**A bracketed IPv6 host could not be used at all.** `postgres://u@[::1]:5432/d` was refused with `bad port in database URL: :1]:5432` -- `split_once(hostport, ":")` cut at the address's first colon, making the host `[` and the port everything after. The brackets exist precisely to keep those colons out of that split. They are also not part of the address, which is worth having measured rather than assumed: `net.connect("::1", p)` answers "connection refused" and `net.connect("[::1]", p)` answers "host not found", so `host` has to hold the bare form for the driver to resolve it.

**A port outside 1..65535 parsed.** `0`, `-1` and `70000` all became the port and were handed to `net.connect`, which failed -- reporting a connection problem for what is a typo in a URL. Those two read very differently when something is down. Only a port too large for an `int` was caught, and only because `parse_int` refused it.

**A query parameter with no `=` was dropped in silence, and the silence was the problem.** Both drivers read `sslmode` through `url_param` and default it to `disable` when the parameter is absent (`pg.zm:182`, `mysql.zm:337`). So `?sslmode` -- a plain typo for `?sslmode=require` -- was not a no-op: it was an unencrypted connection, with nothing logged and nothing returned. A database URL is configuration read once at startup, so it now fails there. The empty pieces that a trailing `?`, a trailing `&` and a `&&` leave behind are still skipped, because those are an ordinary reading rather than a mistake.

**Only the value of a query parameter was percent-decoded, not the name.** `?a%2Db=1` produced a parameter literally named `a%2Db`, so `url_param(u, "a-b")` answered `None` and the only way to find it was to ask for the encoded spelling -- asking for the name as the URL writes it was the one way to miss it.

The readers around it needed nothing. Their cross-dialect coercions are deliberate and documented -- `sql_as_bool` takes an `Int` of 0 or 1 because SQLite has no boolean, `sql_as_float` takes an `Int`, `sql_as_decimal` takes `Text` because that is how a decimal crosses the driver boundary -- and the asymmetry that `sql_as_int` does *not* take a `Float` is right: widening is safe, narrowing is a choice the caller should make.

Checked before tightening anything: every database URL in both repositories and all the test harnesses, 39 of them. None uses a bare `?flag`, a port outside the range, or a bracketed host, and the four URL messages the pg and mysql integration suites pin (`unsupported sslmode`, `has no user name`, `bad port in database URL: notaport`) are all ones this does not touch -- those suites need live servers and do not run in `make check`, so that was worth reading rather than assuming. `mysql://root:secret@127.0.0.1:1/zimandb` is why the range starts at 1 rather than at 1024.


## `sqlite`: SQLite's TEXT is not a Ziman string (2026-10-25)

The driver's careful parts are genuinely careful. Floats cross as `%.17g` in *both* directions -- `sqlite::float_to_text` exists precisely because Ziman's `str(float)` is six significant digits and would quietly round a value on its way to a column -- and the round trip is exact for 0.1, `0.30000000000000004`, 1e308, 5e-324 and pi, checked value by value. Integers survive to the `int64` edges. Error classification is right where it matters: a UNIQUE or NOT NULL violation is `Constraint`, a missing column or table or a syntax error is `Sql`, a database that will not open is `Connection`, and a parameter-count mismatch is caught rather than passed to SQLite. Blobs are hex-encoded, so bytes with NULs in them travel fine.

**What was wrong sat in the one place the type system cannot help: the bytes SQLite calls text.** SQLite is typeless and stores whatever it is given. `'a' || char(0) || 'b'` really is three bytes -- its own `hex()` says `610062`, while its own `length()` says 1, stopping at the NUL -- and `CAST(x'ff' AS TEXT)` really is an 0xff. A Ziman string is valid UTF-8 and holds no NUL, so neither value can become one. The driver handed them over anyway, and failed differently each time:

* **The NUL truncated the value in silence.** `prefixed` memcpy's every byte and `value_string` then measures the result with `strlen`, so `a\0b` arrived as `a` with nothing said. Silent data loss, in a driver, with the database itself able to prove the bytes were there.
* **The invalid UTF-8 killed the program.** It reached `decode_cell`'s `substring`, which raised `string is not valid UTF-8 (invalid sequence at byte 1)` as a **fatal runtime error from inside the driver** -- not a `DbError`, so nothing could catch it. A `SELECT` over a table holding one such row took the process down.

Both are refused in the native now, where the bytes are still in hand, as an `E` cell that `decode_cell` turns into an ordinary `DbError`. **The message names the remedy rather than the rule**: `CAST(col AS BLOB)` reads the column losslessly, and the fixture proves it -- the same value comes back as `[97, 0, 98]`, every byte intact. A reader who hits this wants their data, not a lecture about UTF-8.

**Three drivers, three different answers to the same question**, which is the finding worth carrying past this module:

* `sqlite`, now: refuse with a `DbError` that names the remedy.
* `mysql` (`mysql.zm:629`): fall back to `SqlValue.Bytes(raw)` when `bytes_to_string` fails. Lossless and never fatal, but it changes the column's type without saying why, so a caller matching on `SqlValue` silently takes a different branch.
* `pg` (`pg.zm:124`): `unwrap(bytes_to_string(..))`, which is the fatal shape `sqlite` just lost.

Postgres validates text against the database encoding on the way in, so the server should never send a `text` field that is not valid UTF-8 -- `pg`'s exposure is far smaller than `sqlite`'s, where any column can hold anything. But an `unwrap` on bytes that arrived over a socket is a bet on the peer, and `text_of` is used for protocol parsing throughout that module, not only for column values. **Flagged rather than changed**: the pg and mysql suites need live servers and do not run in `make check`, so a fix there is one I could not have tested, and shipping an untested change to a driver's decode path to close a hole the server is supposed to prevent is the wrong trade. Which of the three answers all three drivers should give is a decision, not a bug fix.


## `pg`: a five-byte header that stopped the client, and a flag I had raised wrongly (2026-10-25)

This pass began by retracting something. The `sqlite` entry above flagged `pg.zm:124` -- `unwrap(bytes_to_string(..))` in `text_of` -- as the same fatal shape `sqlite` had just lost, an `unwrap` on bytes that arrived over a socket, used in twelve places including error field values, column names and column values. **It is safe, and `pg` makes it safe itself.** `startup_message` sends `client_encoding` as `UTF8`, so the server is obliged to convert on output and to fail rather than send anything else.

That is a claim worth testing rather than reading, so it was tested: a `SQL_ASCII` database (which accepts arbitrary bytes, unlike a UTF8 one) holding a `text` value whose bytes are `ff41`, confirmed by `encode(convert_to(v,'SQL_ASCII'),'hex')`. Reading that row gives `database error: invalid byte sequence for encoding "UTF8": 0xff` -- **the server refusing, as a clean `DbError`**, and the client never sees the bytes. The guarantee is real and it is one line of the startup packet.

**What was actually wrong was the length word.** A message is a one-byte kind, a four-byte length, and a body, and `net.read_exact` allocates what it is told before reading a byte. Five bytes claiming `0xFFFFFFFF` made the client wait for 4 GB that was never coming: **measured at 40 seconds and still going**, killed by the test's own timeout rather than by anything the driver owns. Peak RSS stayed at 25 MB, so the reservation never committed -- this is a hang, not an exhaustion.

`db.reply_timeout_millis` does not cover it, and its own comment is why: it is a liveness backstop for a serve thread that died, "not a statement timeout", deliberately not a socket timeout. The right answer is not a timeout anyway, because the length is checkable the moment it arrives. No legitimate message reaches a gigabyte -- a Postgres value cannot, the varlena format stops it -- and libpq defends itself the same way, having "long distrusted large length words except for a small number of message types that are expected to be (potentially) long" as a guard against exactly this, lost message-boundary sync. 40 seconds became 0.02.

**One cap, where libpq has two, and the reason is measurable.** libpq uses a small limit (10000 bytes) for message types that should never be long and the large one only for those that can be, which also catches a desync claiming a merely implausible size. Copying that blindly would have been wrong: a 4 MB text value read back correctly in this pass, and a DataRow carrying it is a legitimate message three orders of magnitude past the small cap. The shape being caught here is a peer that is not Postgres, or a stream that has lost its place entirely, and one cap catches both.

A length *below* four -- the four bytes the word counts for itself -- used to reach `read_exact` as a negative count and come back as `read_exact: count must not be negative`: a complaint about an argument, for what is a peer sending nonsense. It is the same message as the oversized case now, because it is the same problem.

**And a silent truncation in `bytea`.** The hex loop runs `while i + 2 <= len(hex)`, so an odd number of digits lost the last one without a word. Postgres does not send such a value, which is precisely why nobody would ever notice if something else did.

**The difference from the `sqlite` pass is that this one could be verified, and that changed what shipped.** There I flagged `pg` and declined to touch it, because "the pg and mysql suites need live servers and do not run in `make check`". That was true of `make check` and false of this machine: `docker` and `podman` are both installed and the `postgres:17` image was already present, so `make test-pg` runs -- 11 tests, passing before the change and passing after, including TLS, SCRAM, pooling and cursors. On top of that: a 4 MB value, a `bytea` round trip of `[0, 1, 255, 97, 0]` byte-identical through a NUL, a unicode error message classified as a constraint violation, and four shapes of wrong peer (an HTTP response, 1 KB of every byte value, a silent accept, a bogus header) each answered with a `DbError` and a live program. **"I cannot test this" deserves one command's worth of checking before it becomes a reason not to fix something.**

`tests/lang/pg_message_length_cap.zm` keeps the defence honest without a container: eleven bytes from a `net.listen` thread stand in for Postgres, so it runs in `make check` rather than only in `make test-pg`.


## `mysql`: an endless stream of full packets, and a cap I got wrong first (2026-10-25)

MySQL splits a payload of 2^24-1 bytes or more across several packets, each of the maximum length with a shorter one last, so `read_packet` keeps reading while a packet is exactly full. It kept reading **however many arrived**. A peer sending full packets forever makes the client assemble forever: measured at 49 MB of resident memory for one such packet and 213 MB for six, linear and unbounded, with the content rejected only *after* all of it had been assembled -- the driver answered "unsupported MySQL protocol version 0" having first taken 213 MB to reach that conclusion.

**The first cap was wrong, and only measuring caught it.** `max_packet_size = 16777216` sits in the login packet and reads exactly like the answer: the client telling the server its limit, so hold the server to the number we sent. That field is what the client will *send*, and MySQL does not apply it to result data. `SELECT repeat('x', 20000000)` against a server with `max_allowed_packet=64M` returns 20,000,000 characters, and a 16 MB cap refused it -- a legitimate row, broken by a defence against a hostile one. Confirmed in both directions: the same query read fine with the cap reverted.

**1 GB is where MySQL itself stops**, which is what makes it the right number rather than a guess. `SET GLOBAL max_allowed_packet = 2147483648` comes back as 1073741824, so no server can be configured to send more. A hostile peer streaming 1.17 GB is now refused after ~1 GB (peak RSS 1068 MB) in 0.61 seconds, where before it had no end at all. **The cap bounds the damage rather than making it small**, and that is the honest description: a tighter bound would have to refuse rows a real server legitimately sends, so a configurable one is a feature -- next to a statement timeout, as `db.reply_timeout_millis`'s own comment argues -- and not something to hard-code here.

**No fixture, and the reason is the point.** `pg`'s equivalent defence got one, because eleven bytes from a `net.listen` thread stand in for Postgres. The cheapest honest test of this cap is a gigabyte of traffic, which does not belong in `make check`. It is covered by measurement -- 1, 6 and 70 full packets, the numbers above -- and by `make test-mysql`, 6 tests passing before and after, which covers the legitimate multi-packet path that the cap must not disturb.

**A retraction, smaller than the last one.** The `sqlite` entry described `mysql.zm`'s `text_or_bytes` -- falling back to `SqlValue.Bytes(raw)` when the bytes are not valid UTF-8 -- as changing a column's type "without saying why", listing it as one of three drivers answering one question three ways. That was unfair to it. `is_binary_column` decides first and decides properly, on the column's charset (63, which MySQL reports for binary columns) together with the BIT and GEOMETRY types, so a BLOB is `Bytes` because the server *said* it was binary. `text_or_bytes` only runs for a column that claimed a text charset while carrying bytes that are not text, which is the server breaking its own contract after the driver asked for `utf8mb4` in the login packet. Falling back losslessly there is a reasonable answer, and the three drivers are not as inconsistent as that entry made them sound.

While there: `tests/run_mysql_tests.sh` said its container listens on 55433 in the comment and bound 55434 in the code. 55433 is a port this session had just used for a probe container, which is exactly the collision the comment exists to prevent.


## `migrate`: one missing `*/` applied half a migration and called it done (2026-10-25)

`migrate.zm` is one of the better-reasoned modules in the library and most of what it claims, it does. Versions order numerically, so `V10` follows `V2` rather than preceding it. `check_sequence` refuses a gap and a repeat with precise messages (`version 1 is used by both V1__also_one.sql and V1__one.sql`). A `.sql` file that does not fit the name is an error rather than a skip. Version 0 is refused, a missing directory is refused, an empty one applies nothing. The checksum is 44 characters of base64 SHA-256 and changes on a single added space. All the pending migrations and their `schema_migrations` rows go in one transaction.

The statement splitter is the intricate part and it was nearly right: a `;` is protected inside `'strings'`, `"identifiers"`, `--` and `/* */` comments, `$$` and `$tagged$` bodies, and -- the detail that shows someone thought about it -- a doubled `''` needs no special case, because the run ends at the first of the pair and the second opens another, so the text between stays quoted either way.

**Two things it got wrong, one of them badly.**

**An unterminated construct ran to the end of the text.** `quote_end` and `dollar_end` answered `len(sql)` when they found no closer, and the `/*` branch set `i = n`. For a string or a dollar body that is survivable: the unterminated text reaches the server, which refuses it. For a comment it is not, because a comment's content is *discarded*:

```
CREATE TABLE kept (x int);
/* a note that forgets to close
CREATE TABLE lost (y int);
```

applied one statement, never created `lost`, and **reported success** -- then wrote a `schema_migrations` row holding a checksum of the whole file. Nothing would ever flag it, the migration would never run again, and a developer who closed the comment would be met with a checksum mismatch instead of a fix: the repair path is blocked by the module's own integrity check. For a header that promises "a typo cannot silently skip a migration", a half-applied migration recorded as done is the worst available outcome, and one missing `*/` was enough.

It is an error now, naming the line the construct opened on, which is why `split_statements` answers a `Result`. That is a breaking change to a `pub` function with three call sites outside the module, all updated; the alternative -- a separate validating function -- would have left the trap in place for anyone who called the obvious one. The fixture ends by proving the consequence rather than only the message: the migration is refused before anything is applied, so not even `kept` lands.

**MySQL's identifier quotes were not identifier quotes.** The header has always said a `;` inside an identifier does not end a statement, and `"a;b"` was honoured while `` `a;b` `` was split in two -- in a module with a `migrations/mysql/` directory that asks the connection for its dialect. Backticks join the quote set, doubling included.

**Left as a known difference, written down rather than discovered.** Block comments do not nest here, which is SQLite's and MySQL's reading; PostgreSQL's nest. A nested comment is therefore mis-split rather than silently truncated -- the text after the inner `*/` goes to the server, which refuses it -- so the failure is loud, and making it dialect-dependent would mean threading a dialect into a function that is otherwise pure text.

A note on the test that found the second bug: declaring a local `fn split(label, sql)` in the fixture collided with the prelude's `split(s, separator)` and the ambiguity was reported *inside `lib/prelude/string.zm`*, at its own use of `split`. That is the cost the `test` entry above describes for short names -- "a module that declares its own `to_json` binds the name" -- showing up from the other direction, and it is worth knowing that the error surfaces in the prelude rather than at the declaration that caused it.


## `datasource` stops knowing which databases exist (2026-10-25)

`datasource.zm` used to begin with `use mysql`, `use pg`, `use sqlite` and carry an enum of the three concrete connection types:

```ziman
pub enum Connection { Sqlite(sqlite.Connection), Postgres(pg.Connection), Mysql(mysql.Connection), Pool(PoolState) }
```

Its own header explained why: *"There are no interfaces in Ziman to abstract over drivers with; dispatching by URL scheme inside this module is the substitute."* That was the claim this change tested, and it is false. **An engine is now a value an application hands over**, and the module names none of them.

```ziman
val drivers = datasource.registry([sqlite.driver(), pg.driver()])
val db = try datasource.open_pool(drivers, url, 10)
```

**The encoding is a record of closures, and one detail makes it work.** `db.Conn` holds `close`, `execute`, `query` and `rows` as function fields that *already hold* the driver's own connection, so there is no type for a `sqlite.Connection` to be named by and none has to exist. `close` has to live on the connection rather than on the `Driver` -- a `close(connection)` on the driver would need a parameter type for the very thing the closure hides. That is the only forced move in the shape; everything else was a choice.

`db.Driver` is the engine: `name` (which is also the dialect, one field rather than two so they cannot drift), the URL `prefixes` it claims, `poolable(url)`, `idle_check`, and `open(url) -> Result<Conn, DbError>`.

**Eight places in `datasource` knew an engine, not five.** The five `switch` arms were the obvious ones. The other three were the interesting ones, because each was a rule rather than a dispatch:

* the URL-scheme map in `open`, including a `substring(url, 7, len(url))` that silently meant "strip `sqlite:`" -- each engine now owns its URL spelling and strips its own prefix;
* `starts_with(url, "sqlite::memory")` in `open_pool_with`, because an in-memory database is private to its connection and a pool of four would be four different databases -- now `driver.poolable(url)`;
* `Connection.is_Sqlite(c)` in `checked`, because a connection over a socket can be closed from the far end while it sits idle and an open file cannot -- now `driver.idle_check`.

Those three are why a `Driver` has properties and not only functions. A vtable of operations alone would have left all three behind.

**Two bugs fell out.** `dialect` on a pool used to re-read the URL and answer `"postgres"` for any scheme it did not recognise, because the pool held no connection to ask; it holds its `Driver` now and answers `driver.name`. And `migrate` asked `Connection.is_Postgres(tx)` for its advisory lock, reaching through an enum it had no business knowing; it asks `dialect(tx) == "postgres"`.

**What the language allows, and where it pushed back.** Function-typed struct fields, closures capturing a private struct, and a module-level `val` holding a registry of them all work. A module-level **mutable** registry does not, and the error is a design statement rather than a limitation: *"`Mutable<[Driver]>` cannot be the type of a module-level val (every thread would share it)"*. That settled the "kept somewhere" question before it could be argued -- the registry is a value an application builds and passes, not a global drivers register, and self-registering drivers were never on the table.

**Call-site arithmetic, which is why this was affordable.** 89 sites call `open`/`open_pool*` and needed the registry; 511 call `query`/`execute`/`rows`/`with_transaction`/`close`/`dialect` and take a `Connection`, so they did not change at all. Of the 89, **three are in the real application** -- and all three now read `datasource.open_pool(drivers, ..)` with one `pub val drivers` in `common/common.zm`, the single place the sample backend decides it links SQLite and PostgreSQL. The rest are test setup.

**The sweep script had to be rewritten twice, and the dry run is what saved it.** The first version matched `datasource.open` inside *comments*, and its edit list included `lib/db/datasource.zm`, `pg.zm`, `sqlite.zm` and `mysql.zm` -- it would have added `use pg` back to `datasource` and undone the entire change on the same commit that made it. The second version stripped comments by cutting each line at `//`, which cuts `postgres://` in half, so it mistook a Postgres test for a SQLite one. Only the third, which skips string literals while stripping comments, was right. **A migration script's dry run is not a formality when the script's own subject is the text it edits.**

One test got better rather than merely fixed. `db_pool_sqlite` proved the pool hands out distinct connections by reading `Connection.unwrap_Sqlite(tx).handle` -- a C handle, through two layers that no longer exist. It now builds a per-connection identity out of SQLite's own `TEMP` schema, which belongs to the connection that created it, so the first borrow stamps a connection and every later borrow of it reads the same stamp. The assertion is unchanged and it now tests a property of the pool rather than of SQLite's internals.

Verified on every engine, which this refactor deserved: `make check` (1196 lang tests plus the sample backend's own suite), `make test-pg` (11 against a real PostgreSQL 17), `make test-mysql` (6 against a real MySQL 8), and `make northstar-both`, which runs the whole sample application's behavioural suite against **sqlite and postgres in turn** -- 0 failed on both, through the new registry. The dependency graph is the short version: `datasource --> clock` and `datasource --> db`, and nothing else.


## `memdb`: a SQL engine in Ziman, and a tier boundary drawn in the wrong place (2026-10-25)

1,943 lines of Ziman that lex, parse and run SQL over tables held as immutable values, reached through `db.Driver` like any other engine:

```ziman
val drivers = datasource.registry([memdb.driver()])
val conn = try datasource.open(drivers, "memdb:")
```

It exists because `datasource` stopped naming engines the day before. A fourth engine is a new module and a `driver()`, and nothing above it changed -- which is the first time that claim was tested by something other than the three drivers that were already there.

**Scoped by measuring first.** "Supports SQL" is unbounded, so before writing any of it: the sample application's 191 SQL strings (20,365 characters) use `SELECT` 85, `DELETE` 49, `ORDER BY` 40, `UPDATE` 30, `INSERT` 27, `COUNT` 12, `LIMIT` 11, and **no joins and no subqueries at all**, because its repositories query one table each and compose in Ziman. That is what made this a few days' work rather than a quarter's.

**It is a third target, not a replacement, and that was the important decision.** A suite that runs only on an engine production never uses certifies the wrong thing, and the divergences are the quiet kind -- NULL ordering, type affinity, `LIKE` case sensitivity, integer division. So `tests/lang/memdb_against_sqlite.zm` gives both engines the same schema, the same data and 28 queries, and compares the answers row by row: three-valued logic (`age = NULL` matching nothing, `NOT (age > 30)` dropping the NULL row), integer versus float division, `LIKE` with `%` and `_`, `IN`/`NOT IN`, `BETWEEN`, `CASE WHEN`, the scalar functions, `||`, writes, and a rolled-back transaction. **28 of 28 agree.** A divergence is a changed recording and a failing test, which is the only arrangement that makes a second SQL implementation safe to own.

**Transactions are free because a database is a value.** `BEGIN` keeps the current `Database`, `ROLLBACK` puts it back, `COMMIT` forgets it. No undo log, no copying: the saved value *is* the old state, and the saves are a stack so nesting works. The same property is what will make per-test forking free -- seed a schema and a dataset once, hand each test its own copy.

**The tier boundary was in the wrong place, and the real schema is what proved it.** The agreed first increment was "enough to run schema.sql + data.sql and simple reads", with aggregates, `LIKE`, `IN`, `BETWEEN` and `CASE WHEN` held back for the next one. Then it met the sample application's actual migration directory, and each attempt found one more thing a *real* schema.sql needs:

* `{{uuid}}` had no spelling for a new dialect -- and `migrate`'s token table had a field per engine with a three-branch lookup, which is the shape `datasource` had just lost. It is a `Map<string, string>` keyed by dialect now, so an engine is an entry rather than an edit.
* The migrations live in a shared directory plus one per engine, and `memdb` has neither. No library change: the caller says `load_for(dir, "sqlite")`, because *the application* knows its memdb runs its sqlite migrations and `migrate` should not.
* `SELECT 1` -- no `FROM`. Legal in both real engines, and the liveness probe `datasource`'s own pool sends.
* `||`, then `substr` and `length` (30 calls each, for a data conversion), then `LIKE`, `IN`, `BETWEEN` and `CASE WHEN`.
* All 40 migrations then applied, building a 36-table schema.

The lesson is not that the estimate was bad. It is that **a tier boundary drawn from a feature list and one drawn from a real input are different boundaries**, and only the second one is worth anything. "Simple reads" sounded like a milestone; "runs the actual migration directory" is one, and it sits most of a tier further along.

**What it does not do yet, measured rather than guessed.** Across the application and its tests: `COUNT(` 41, `RETURNING` 2, `GROUP BY` 2, `SUM`/`MIN`/`MAX` 1, `JOIN` 1, one subquery, no `DISTINCT`. So the aggregates are the whole of the next increment -- 41 of the 48 remaining uses are `COUNT`, and the backend's own tests cannot run on `memdb` until they exist. Constraint *enforcement* (`NOT NULL`, `PRIMARY KEY`, `UNIQUE`, foreign keys are parsed and remembered, never enforced), `JOIN`, subqueries and per-test forking come after.

Divergences it knows about are in its header rather than discovered later: no type affinity, NULLs sort first ascending (SQLite's order, not PostgreSQL's), `LIKE` case-insensitive for ASCII (SQLite's rule, not PostgreSQL's), division by zero answers NULL (SQLite's answer, where PostgreSQL raises), `CREATE INDEX` accepted and ignored because an index decides speed and not answers, and no `sqlite_master` or `information_schema` -- a test that asks the engine about itself is asking the wrong question of this one.

Three things the language said no to, each for a stated reason. A lambda cannot capture a `var` ("captures are by value, so the lambda and the enclosing function never share mutable state"), so the `ORDER BY` comparator carries its failure out in a cell. `try` cannot sit on the right of `&&` ("it must run exactly once and unconditionally"), so a `CASE` arm evaluates its condition in its own step. And `if V(x) = e` is a statement form, not an expression, so a test for a payload-free variant is `SqlValue.is_Null(v)`.


## `memdb` learns the aggregates, and two readings of SQL (2026-10-25)

**ANSI SQL is real -- ISO/IEC 9075, current edition SQL:2023 -- and conformance is not on the table.** Core SQL, the standard's own minimum bar, already wants views, cursors, `GRANT`/`REVOKE`, isolation levels, `information_schema`, full outer joins, set operations, quantified subqueries and enforced constraints. Claiming "ANSI SQL" for a 2,400-line module would be the kind of sentence these documents exist to prevent. What is worth having is the standard deciding *semantics* inside the subset that is implemented, instead of behaviour copied from reading SQLite.

**The aggregates are where that pays immediately**, because their NULL rules are the standard's, both real engines follow them, and they are the ones a reader guesses wrong:

* `COUNT(*)` counts **rows**, including a row whose every column is NULL; `COUNT(expr)` counts rows where `expr` is not NULL.
* Every other aggregate **ignores** NULLs rather than propagating them -- the one place in SQL where a NULL does not poison the result.
* Over no surviving value, `SUM`, `MIN`, `MAX` and `AVG` are **NULL** while `COUNT` is **0**. `SELECT SUM(x) FROM empty` is NULL and `SELECT COUNT(x) FROM empty` is 0, and that asymmetry is worth having written down rather than rediscovered.
* With no `GROUP BY` there is one group over every matching row *including none*, which is what makes `SELECT COUNT(*) FROM empty` answer one row of 0 rather than no rows. With a `GROUP BY` and no rows there are no groups and no rows -- the other half of the same rule.
* Two NULL group keys group **together**, although `NULL = NULL` is unknown: grouping compares for distinctness, not with the equality operator.

`GROUP BY`, `HAVING`, `COUNT`/`SUM`/`MIN`/`MAX`/`AVG`, aggregates inside expressions and `ORDER BY count(*)` all agree with SQLite -- 42 of 42 queries in the cross-check fixture, up from 28.

**Two readings of SQL, one engine.** `driver()` at `memdb:` reads SQL as SQLite does; `strict_driver()` at `memdb-ansi:` reads it as the standard does. They differ in exactly three places, which are the three where SQLite departs from ANSI: `LIKE` folds ASCII case (the standard compares by collation), division by zero answers NULL (the standard makes it error 22012), and a number may be compared with text by a type rank (the standard refuses). One `strict` field on the evaluation context, marked at each of the three decisions, so the two readings cannot drift apart in separate code.

**The strict one is a portability check, and that is the whole reason it exists.** The sample application ships on SQLite *and* PostgreSQL, and this file records a two-commit PostgreSQL outage caused by exactly that gap. Where SQLite departs from the standard, PostgreSQL does not -- so a query that passes on `memdb:` and fails on `memdb-ansi:` is leaning on SQLite and will behave differently in production. Finding that costs a second local run rather than a container. `SELECT id FROM t WHERE name = 1` is the one to remember: SQLite matches nothing and says nothing, and the strict engine answers "cannot compare TEXT with INTEGER".

The fixture asserts **both** halves, which is what keeps the flag honest: the lax reading still matches real SQLite on all six divergent queries, the strict reading still differs on all six, and the two agree on twelve others. Either half failing means the flag has stopped meaning anything -- the lax one by drifting from SQLite, the strict one by checking nothing.

**The cross-check earned its keep within a minute of the aggregates existing.** `SELECT count(age) FROM visit WHERE reason = 'nothing'` answered `0` on `memdb` and `no such column: age` on SQLite, because `age` is not a column of `visit` and the aggregate looked columns up *as it read each row* -- so a `WHERE` that matched nothing never looked. A real engine resolves columns when it prepares a statement, and now so does this one: a pass over the select list, filter, groups, having and order terms before anything runs. **A typo'd column name passing here and failing on the real engine is the precise false green this arrangement is for**, and it was found by the mechanism built to find it rather than by a reader.

One duplication bug, worth recording for its shape. `GROUP BY grp` failed with "unexpected 'BY' after the statement", because the two places that accept a bare alias each spelled out the clause keywords that cannot *be* an alias, and the second spelling was missing `GROUP` and `HAVING` -- so `FROM t GROUP BY grp` read `GROUP` as the table's alias. The list is one `val` consulted twice now. Two copies of a keyword list is one copy too many, and the second copy is always the stale one.


## `sql`: the statement tree moves out of the engine, and two precedence bugs fall out (2026-10-25)

`lib/db/sql.zm` -- a `Statement` tree, a parser that reads one, and a formatter that writes it back. No database, no connection, no engine. `memdb` is built on it and keeps only storage, evaluation and its two drivers; 1,621 lines moved out of it, leaving 1,556.

```ziman
val statement = try sql.parse("SELECT name FROM patient WHERE id = $1")
print(sql.format(statement))
for s in try sql.parse_file("migrations/V001__create_users.sql") { ... }
```

**The module was already written and in the wrong place.** memdb held a lexer, a token cursor, an AST and a recursive-descent parser -- the whole of which is a SQL front end that has nothing to do with running a database. Moving it is the split the two names already implied: **`sql` is syntax, `memdb` is semantics.**

Two smaller things fell out of the move. README.md and SYNTAX.md had both listed a `sql` module in the database layer **for as long as that table has existed**, and there was no such file; `check_doc_modules.py` missed it because it validates `module.member` spans and `use` lines, and `sql` appeared only in prose. And `Statement` is named `Statement` rather than `SqlStatement`, because qualified it reads `sql.Statement` where `sql.SqlStatement` would stutter exactly as `random.random_bytes` and `uuid.uuid_new` were weighed for.

**`parse(format(parse(x))) == parse(x)` is the point of having a formatter**, and it earns its keep immediately. `tests/lang/sql_round_trip.zm` asserts it over 40 hand-written statement shapes and then over the sample application's own migration directory, read off disk and passed through `migrate.substitute` first so `{{uuid}}` is resolved the way the real pipeline resolves it: **39 files, 132 statements, 0 that do not settle.**

**It found two precedence bugs, and both changed what a query means rather than how it looks.**

* **`NOT` was parsed as a prefix of a primary**, so `WHERE NOT a = 1` read as `(NOT a) = 1`. SQL puts `NOT` *looser* than a comparison: it means `NOT (a = 1)`. Checked against SQLite rather than against the standard's prose -- `SELECT a FROM t WHERE NOT a = 1` over rows 1 and 2 answers one row, which is `NOT (a = 1)` and not `(NOT a) = 1`.
* **`IS NULL` was parsed as a postfix on a primary**, so `1 + NULL IS NULL` read as `1 + (NULL IS NULL)`, which is 2. SQLite answers 1, because `IS NULL` is a predicate and binds looser than arithmetic: `(1 + NULL) IS NULL`.

Neither is the kind of bug reading the parser finds, because each looks right where it is written. The round trip found them the same way: a tree printed as text that parses back to a *different* tree. The ladder is now one comment away from the code that implements it -- `||`, then `* /`, then `+ -`, then the predicates, then `NOT`, then `AND`, then `OR` -- and the precedence numbers the formatter uses are the same constants.

**Preserved rather than modelled.** A formatter that quietly dropped a `FOREIGN KEY` would be worse than no formatter, so the parts the tree does not model are kept as the text they were written as and written back verbatim: a column constraint beyond `NOT NULL`/`PRIMARY KEY`/`UNIQUE`/`DEFAULT`, a table-level constraint, and a pass-through statement (`PRAGMA`, `SET`, `ANALYZE`, `VACUUM`). Tokens carry their source span for it, which is also what makes the preservation exact rather than re-assembled with guessed spacing. Three things the parser used to discard are now modelled properly instead: a qualified column reference (`t.c` kept its name and threw away its table), a table alias, and `CREATE INDEX`, which was swallowed whole because the engine has no use for one -- a formatter does.

**`parse_many` splits on `;` *tokens*, not characters**, which is the one real advantage of having a lexer: a `;` inside `'a;b'`, inside `"a;b"`, or inside a `--` or `/* */` comment cannot be mistaken for a separator. `migrate.split_statements` stays where it is and keeps its text-level scan, because it must divide SQL this module may not be able to *parse* -- engine-specific DDL included. Same word, different job.

**A fourth divergence between the two readings, found by the same fixture.** `SELECT id = 1 AS yes` came back as a boolean from `memdb` and as `Int(0)`/`Int(1)` from SQLite, because **SQLite has no boolean type** while PostgreSQL and the standard have one. So the lax reading now renders a projected boolean as 0 or 1 and the strict one does not -- which is also why `db.sql_as_bool` has always accepted an `Int` of 0 or 1. Only the projection converts; inside a `WHERE` a boolean stays a boolean, because nothing outside sees it.

**What this is not.** It is not ANSI SQL. ISO/IEC 9075 also has joins, set operations, subqueries, `DISTINCT`, common table expressions, window functions, `MERGE`, `GRANT`/`REVOKE`, views, schemas, sequences, domains, triggers, routines, cursors, savepoints, isolation levels, `GROUPING SETS`, `LATERAL`, interval and row constructors, `IS DISTINCT FROM`, `SIMILAR TO`, `OVERLAPS`, temporal tables and SQL/JSON. PostgreSQL's own grammar is some twenty thousand lines and is not full ANSI either. The module says so in its header rather than in a release note, and `#![experimental]` says the tree will reshape as coverage grows -- joins will turn one `table` field into a list of table references, and a subquery will make `Expr` recursive through a `Statement`. The round trip is what makes those changes safe to make.


## `sql` and `memdb` learn joins and subqueries, and validation loses its nerve (2026-10-25)

The last entry predicted the shape of this one: *"joins will turn one `table` field into a list of table references, and a subquery will make `Expr` recursive through a `Statement`."* Both happened as written. `SelectStmt.table: string` is now `from: [TableRef]` with a `joins: [Join]` beside it, and `Expr` has four new variants -- `Sub`, `Exists`, `InSub` and `Cast` -- of which the first three hold a whole `SelectStmt`.

**That makes `Expr` and `SelectStmt` mutually recursive**, an enum through a struct and back, which Ziman allows and constructs without ceremony because `Lit` and `Col` end the chain. Worth recording because it is the first place in either repo that needs it.

**A join is a flat list, not a tree.** `a JOIN b ON .. JOIN c ON ..` is left-deep -- that is what SQL means by it -- so a list of joins onto the first table says the same thing as a tree of pairs and reads better at every use. A parenthesised join expression would need the tree, and is not read. `[INNER] JOIN`, `LEFT [OUTER] JOIN`, `CROSS JOIN`, `RIGHT` and `FULL`, each with `ON` or `USING`; `USING (a, b)` is desugared into the `ON` it stands for rather than carried into evaluation twice.

**`RIGHT` and `FULL` are read by `sql` and refused by `memdb`, with an error naming the engine to use instead.** They are a mirror and a union of what the engine does, and shipping them without a test that proves them against a real engine would be guessing -- the whole point of this engine is that it does not guess. Reading them anyway is right, because a formatter that dropped a `RIGHT JOIN` from a file it was asked to rewrite would be worse than one that refused the file.

**Two new ideas in the engine, and both are about *where a name lives*.**

* A `Source` is what a `FROM` produces: a column name, a *table* and a row for each. The table per column is what makes `id` across two joined tables **ambiguous** rather than silently the left one -- `resolve_column` counts its matches and answers -2 for more than one, and `p.id` narrows it to one.
* A `Ctx` carries the scopes enclosing it, outermost last. A name a subquery does not have falls **outward**, one level at a time, which is the whole of correlation: `WHERE visit.patient_id = patient.id` inside a subquery means what it says, and `EXISTS`, `IN (SELECT ..)` and a scalar `(SELECT ..)` all get it from the same three lines.

`DISTINCT` drops duplicates **after the projection and before the `LIMIT`**, which is the order SQL specifies: `SELECT DISTINCT x .. LIMIT 2` means two distinct rows, not the distinct ones among the first two. Rows compare by SQL's own rules, so two NULLs are the same row -- `DISTINCT` asks about distinctness, not about equality, the same reading `GROUP BY` takes of its keys.

**And the mistake, which is the reason this entry is worth its length.**

`validate_expr` was the fix from the previous entry: resolve every column before the statement runs, so `SELECT count(age) FROM visit WHERE reason = 'nothing'` cannot answer 0 where SQLite says "no such column: age". Correlated subqueries break its premise -- a name may legitimately belong to an enclosing query -- and the loosening I reached for first was this:

```ziman
Col(_) {
    // Columns are resolved against the row's own shape when the statement runs, which a
    // join and a correlated subquery both need -- a name may belong to an enclosing query.
    unit
}
```

Which validates nothing. **The cross-check fixture failed on the same query it had caught the first time**, three commits after the entry describing that catch was written. One line of comment made it sound deliberate, which is how a guard dies: not removed, excused.

The real fix is that validation needs a *shape*, not a row. `shape_ctx` reads the column names and qualifiers of every table in the `FROM` and its joins straight from the table definitions, with no rows touched at all, and the enclosing scopes chain onto it exactly as they do at evaluation -- so a correlated name resolves and a typo does not. A subquery validates against its own shape with the outer one enclosing it, so `EXISTS (SELECT 1 FROM visit WHERE visit.patient_id = patient.nope)` is refused before any row is read.

**Then the fix broke seven queries, in a way worth keeping.** `ORDER BY alias` is legal SQL, so validation also admits the names the select list gives its columns -- and appending those to the source's columns made `SELECT name FROM patient ORDER BY name` report **"ambiguous column name: name"**, because `name` was now both a column and an alias. SQL resolves an `ORDER BY` term against the source *first* and against the select list *second*; a name that is both is the column. Two attempts at one guard, both caught by the fixture within seconds of being written, which is the argument for having it.

**The numbers.** The cross-check fixture runs **80 queries on `memdb` and on real SQLite and all 80 agree**, up from 47: joins of every supported kind including a self-join and a `LEFT JOIN` whose `ON` filters the right side, `SELECT *` across a join, scalar and correlated subqueries, `IN`/`NOT IN (SELECT ..)`, `EXISTS`/`NOT EXISTS`, `DISTINCT`, `COUNT(DISTINCT x)`, `CAST` in four directions, and three queries whose column names are typos and must be refused by both. The round-trip fixture carries **61 statement shapes** settling in one pass, up from 40, plus the sample application's 39 migration files and 132 statements with none that fail to settle. The two readings of SQL still agree on everything outside their four stated divergences -- 20 shared queries now, up from 12.

**One SQLite trap, recorded so nobody reverts the fix.** `SELECT (SELECT cost FROM visit WHERE id = 99) AS nothing` is a syntax error in SQLite: `NOTHING` is a keyword there, from `ON CONFLICT DO NOTHING`, and cannot be a bare alias. The fixture says `AS missing`. That is a difference in the dialects' *keyword lists*, not in the engines, and the distinction matters -- a cross-check that cannot tell those apart would be reporting noise.

**Measured, as before, rather than assumed:** the sample application issues one `JOIN` and one subquery in 191 SQL strings, because its repositories query one table each and compose in Ziman. Both are here anyway. A test suite is not restricted to the queries the application happens to issue today, and an engine that refuses a join pushes every such test back onto a container -- which is the cost this engine exists to remove.


## `RETURNING`, and a clause the standard does not have (2026-10-25)

`INSERT`, `UPDATE` and `DELETE` each gained a `returning: [SelectItem]`, read by `sql` and run by `memdb`. An `INSERT` answers what it stored with defaults filled in, an `UPDATE` the row as it now stands, a `DELETE` the row as it was -- which is the only version of a deleted row there is. **17 `RETURNING` statements run on `memdb` and on real SQLite and all 17 agree**, covering `*`, a column list, aliases, expressions over the written row, a multi-row insert, a statement that matches nothing, and a typo'd column that both must refuse.

**A `RETURNING` clause is a select list, so it is the same code.** `parse_items` and `format_items` are now one parser and one writer shared with `SELECT`, where the select list's version had been inline. That is the same argument `clause_words` won earlier in this file -- two copies of a list is one copy too many, and the second copy is always the stale one. `clause_words` itself gained `RETURNING`, so `UPDATE t SET a = 1 RETURNING a` cannot read `RETURNING` as an alias the way `FROM t GROUP BY grp` once read `GROUP` as a table's.

**It is not ANSI SQL, and `memdb-ansi:` runs it anyway.** The standard's way to read the rows a write touched is a *data change delta table*: `SELECT * FROM FINAL TABLE (INSERT INTO t ..)`, SQL:2011, with `OLD`, `NEW` and `FINAL` as the result options. Essentially only Db2 implements it -- PostgreSQL has `RETURNING` and not the delta table, SQLite has had `RETURNING` since 3.35, MariaDB has it, and MySQL has nothing of the kind.

So this is the first thing the strict driver reads that the standard does not have, and the decision needs a reason rather than a reflex. **The reason is that whether a connection has `RETURNING` is a question about the engine, not about SQL's semantics.** The four divergences the strict flag exists for are places where *the same query means something different* -- `LIKE`'s case folding, division by zero, a number compared with text, a projected boolean. `RETURNING` is not one of those: it either exists or it does not, and the question is answered by `datasource.dialect`.

The sample application settles it. Its `returns_written_row` already asks exactly that way -- `datasource.dialect(conn)` against a list of engines that have the clause -- and **both memdb drivers report the dialect `memdb`**, so a strict refusal would fail every save and every update in that application on a flag whose job is to find SQL that leans on SQLite. That would be noise reported as signal, which is the one thing a portability check must not produce.

The same gate is the reason for a one-line change in the sample application in the same breath: `returns_written_row` now names `memdb`, **before anything there opens a memdb connection**. A suite running on `memdb` while the gate said no would quietly take the write-then-read path where production takes the `RETURNING` one -- a test exercising the other half of the function it is meant to cover, which is the precise false green this engine was built to prevent.

**And the cross-check found a real limitation on the way, in code that had nothing to do with `RETURNING`.** `RETURNING *, id` was refused with "`'*'` is only allowed on its own in a SELECT list", and SQLite answers it fine. The restriction was in the select list too: `SELECT *, id`, `SELECT id, *`, `SELECT *, *` and `SELECT *, u.id AS uid` across a join were all refused. A star now expands wherever it appears, through one `item_headers` helper shared by the select list and the `RETURNING` clause, and all five shapes agree with SQLite. **A fixture written for one clause caught a two-month-old gap in another**, which is the argument for cross-checking against a real engine rather than against one's own expectations.

A qualified star (`SELECT t.*`) is still not read -- it errors on both sides with different messages -- and is the next increment rather than a silent wrong answer.

**The numbers.** The round-trip fixture carries 70 statement shapes, up from 61, plus the sample application's 39 migration files and 132 statements with none failing to settle. The cross-check runs 80 read queries and 17 `RETURNING` writes against real SQLite with no disagreement, and compares what is left of the table afterwards. One more case runs the shape the application *actually* issues -- placeholders plus `RETURNING *`, read through `query_one` -- because every other line in that file binds nothing, and a clause that worked only with literals would pass all of them and fail in production. It also checks that `execute` on a `RETURNING` statement still reports the affected count, since both forms read `.affected` and `.rows` from one shared run. The two readings of SQL agree on 23 shared queries, up from 20, three of which are `RETURNING` writes -- so the decision above is a guard and not just a paragraph.


## A qualified star, and the first divergence that points the other way (2026-10-25)

`SELECT t.*` -- a star naming one table's columns. `Expr` gained `TableStar(string)` rather than a qualifier on `Star`, so `count(*)` and every other reader of a bare star is untouched; `memdb` resolves it against the qualifier each column was given by the `FROM`, which is an alias where the table had one. That is what makes `SELECT p.* FROM patient AS p` work and `SELECT patient.* FROM patient AS p` fail, on this engine and on both real ones.

**The enum did the work of finding the call sites.** Adding the variant produced five compile errors and no more: `eval_expr`, `validate_expr`, `has_aggregate`, the nested-aggregate fold and `item_name`, each with the message *"this `switch` over sql.Expr is not exhaustive: missing TableStar (there is no default arm on purpose, so a new variant is a compile error here)"*. The no-default-arm rule is the reason this increment needed no searching.

A qualifier naming no table in scope is an error rather than an empty expansion -- `SELECT x.* FROM t` is a mistake, and answering no columns would hide it. `memdb` says "no such table: x", which is SQLite's wording too, so the cross-check reports agreement rather than a near miss. `table_known` makes the same outward walk through enclosing scopes that `column_known` makes, because `t.*` inside a correlated subquery may name a table of the enclosing query -- and the fixture has that case.

**Then `RETURNING t.*` turned out to be the first place the two readings part in the opposite direction.** SQLite refuses it outright -- `RETURNING may not use "TABLE.*" wildcards` -- and PostgreSQL answers it. That was **measured rather than assumed**: a throwaway `postgres:17` container, four statements, removed afterwards. The same probe settled three neighbouring questions: `SELECT *, t.*` works on PostgreSQL as on SQLite, `SELECT zz.* FROM zz AS p` is refused by PostgreSQL as by SQLite ("Perhaps you meant to reference the table alias"), and `count(t.*)` -- which SQLite and this engine refuse -- *counts whole rows* on PostgreSQL. The last is why `count(t.*)` is listed as not read rather than left to be discovered.

So the lax reading refuses `RETURNING t.*` in SQLite's own words and the strict reading runs it, which makes it **the fifth divergence and the only one whose arrow points the other way**. The first four are places where the strict reading refuses what SQLite accepts, and the rule for reading a failure was simple: passes on `memdb:`, fails on `memdb-ansi:`, therefore leans on SQLite. The fifth adds a mirror of that rule -- fails on `memdb:`, passes on `memdb-ansi:`, therefore PostgreSQL-only and will break on SQLite. Both are worth knowing, and the asymmetry is in both drivers' headers rather than left for a reader to infer from the code.

Writing it the other way was tempting and would have been wrong. Refusing `RETURNING t.*` in both readings is less machinery, and it would have meant `memdb-ansi:` no longer modelled the engine it exists to stand in for.

**The numbers.** 93 read queries and 21 `RETURNING` statements agree with real SQLite, up from 80 and 17; the new ones include a qualified star across a join, both sides of a self-join, `*, p.*` together, inside `EXISTS`, through a `LEFT JOIN`'s unmatched rows, shadowed by an alias, and with a qualifier that names nothing. 76 round-trip shapes settle in one pass, up from 70. The strictness fixture holds 10 divergences, each still matching SQLite on the lax side and still differing on the strict side.


## Set operations, and a query whose answer depends on the engine (2026-10-25)

`UNION`, `INTERSECT`, `EXCEPT`, each with `ALL`, chained, with the compound's own `ORDER BY`, `LIMIT` and `OFFSET`. **121 read queries and 21 `RETURNING` statements agree with real SQLite, up from 93 and 21; 89 statement shapes round-trip, up from 76; the two readings now differ in seven places, up from five.**

**`SelectStmt` gained `set_ops: [SetOp]` -- a flat list, as joins are, but for a sharper reason than style.** The grouping of a chain is dialect-specific, and that is this increment's one genuinely dangerous finding:

* **SQLite reads `UNION`, `INTERSECT` and `EXCEPT` strictly left to right.**
* **The standard and PostgreSQL bind `INTERSECT` tighter than the other two.**

So `a UNION b INTERSECT c` is `(a UNION b) INTERSECT c` on one engine and `a UNION (b INTERSECT c)` on the other. Both were measured, with a dataset chosen to tell them apart -- a={1,2}, b={2,3}, c={3}, giving {3} on SQLite and {1,2,3} on PostgreSQL. **Two answers, no error, no warning, and nothing in the text of the query to say which you are getting.** A tree built in `sql` would have to pick an engine, and `sql` has no dialect; so the parser records the sequence as written and `memdb` groups it -- left to right in the lax reading, with one pass over the `INTERSECT`s first in the strict one. Which is the same split this module pair was built on: **`sql` is syntax, `memdb` is semantics**, and here the difference between the engines is semantic even though it looks syntactic.

Three smaller divergences came with it, all in the direction where **SQLite is the stricter engine**: `INTERSECT ALL`, `EXCEPT ALL` and `UNION DISTINCT` are syntax errors in SQLite and ordinary SQL in PostgreSQL. `sql` reads all three because one of the two engines does; the lax reading refuses them in SQLite's own words, down to `near "ALL": syntax error`, so the cross-check reports agreement rather than a near miss. The `ALL` forms are counting operations in the strict reading, measured against PostgreSQL: `INTERSECT ALL` keeps a row as many times as it appears in the side it appears in least, `EXCEPT ALL` removes one occurrence per occurrence on the right.

`UNION DISTINCT` is why **`SetOp` keeps the quantifier as the word it was written with** rather than as an `all: bool`. `DISTINCT` after a set operator changes nothing about the answer -- it is the default -- so a flag would have been the obvious shape, and it would have let the formatter write `UNION` where `UNION DISTINCT` was read. That is not a cosmetic normalisation: it turns SQL that SQLite rejects into SQL that SQLite accepts. A formatter may tidy whitespace; it may not launder a dialect error.

**And the increment turned up five pre-existing `ORDER BY` bugs, because a compound query cannot be ordered any other way.** A compound has no single source, so its `ORDER BY` may name only an output column or a position -- and neither worked:

| query | `memdb` answered | SQLite answers |
|---|---|---|
| `SELECT n FROM a ORDER BY 1 DESC` | ascending | descending |
| `SELECT n AS x FROM a ORDER BY x DESC` | `no such column: x` | descending |
| `SELECT n FROM a ORDER BY 3` | the rows, unordered | `1st ORDER BY term out of range - should be between 1 and 1` |
| `SELECT n FROM a ORDER BY 0` | the rows, unordered | the same refusal |
| `SELECT n, n * 2 AS d FROM a ORDER BY 2 DESC, 1` | unordered | ordered |

An integer `ORDER BY` term is a **position**, not the number it looks like; evaluating it per row compares 1 with 1 for every pair and silently leaves the rows as they came. The fixture had never used an ordinal or an output name in an `ORDER BY`, so nothing caught it -- 93 queries agreeing is not the same as a feature being right, which is worth remembering about every green suite. The out-of-range message is SQLite's wording down to the `1st`, which needed an English ordinal helper (`1st`, `2nd`, `3rd`, `11th`, `21st`) to match.

Fixing it meant **projecting before ordering**, which is the order SQL specifies anyway: `ORDER BY 2` has nothing to read until the second output column exists. Both the plain and the grouped path now project into a values array, sort a *permutation* of places so a comparator can reach both the projected row and the source row behind it, and build the rows afterwards. A term resolves against the source first and the output second -- `SELECT name FROM t ORDER BY name` orders by the column -- which is the rule the join increment already established for validation.

Two refactors fell out and are worth naming because both were duplication the increment would otherwise have tripled: `values_same` is now the one place a row is compared for *distinctness* (two NULLs are the same row), shared by `DISTINCT`, `GROUP BY`'s keys and every set operation; and `sliced_rows` is the one place `LIMIT`/`OFFSET` is applied, shared by the plain, grouped and compound paths. `parse_subquery` lost an eleven-arm switch that existed only to unwrap a `Statement` -- it reads `parse_query` now, which also means **a subquery may be a compound**, and the fixture has `IN (SELECT .. UNION SELECT ..)` to prove it.

**What is refused, and why that is the right answer for now.** A parenthesised arm -- `(SELECT ..) UNION (SELECT ..)` -- is not read. PostgreSQL accepts it, SQLite refuses it, and recording it would need a tree rather than a list; since the AST would not record the brackets, the formatter would write the flat form and so turn SQL SQLite rejects into SQL it accepts. The same argument as the quantifier, and the same conclusion: refuse it until the tree can hold it. `CORRESPONDING` is not read either, and nothing implements it.

Three statements are now pinned in a small "both refuse, in different words" section of the cross-check: an `ORDER BY` before a `UNION`, `count(t.*)` and `t.* + 1`. A reader deciding whether to trust this engine needs to know that the two agree on what is *legal* even where they do not agree on how to say so -- and `sql` keeps its own voice for a parse error, because it has no dialect to borrow one from.


## Three defects an inventory found, one of them a crash (2026-10-25)

Asked what of ANSI SQL is still missing, I answered by **running** 62 standard constructs through `sql.parse` rather than by recalling what the header claims. Two of the 62 were not gaps at all but defects, and chasing those turned up a third that was worse than either.

**1. `ALTER TABLE t ADD CONSTRAINT k UNIQUE (a)` was read as adding a column named `CONSTRAINT`.** It formatted back as `ALTER TABLE t ADD COLUMN CONSTRAINT k PRIMARY KEY a` -- brackets gone, meaning gone -- and `memdb` **accepted** it where SQLite refuses it outright. A migration written against PostgreSQL would have passed on this engine and failed on both real ones, which is precisely the false green the cross-check exists to prevent.

The round trip could not catch it, and that is the lesson in it: **a wrong tree that re-parses to itself round-trips perfectly.** The property proves the parser and the formatter agree with each other, not that either agrees with SQL. Only a second engine can say that, which is why the cross-check and the round trip are both there.

`AlterStmt` now carries an `AlterAction` enum -- `AddColumn(Column)` or `AddConstraint(string)` -- rather than a column and, implicitly, the hope that it is one. The constraint is kept as the text it was written as, which is the treatment `CREATE TABLE`'s table constraints already get. `ADD COLUMN` settles which case it is; without that word, a constraint keyword does. In `memdb` the lax reading refuses it in SQLite's own words, naming the first word it could not read (`near "CONSTRAINT": syntax error`, `near "UNIQUE": syntax error`), and the strict reading accepts it and records it nowhere -- which is exactly what it already does with a `CREATE TABLE`'s constraints, since it enforces none. Saying so beats pretending.

**2. An unknown function was never refused until something evaluated it.** `SELECT nosuchfunc(a) FROM empty_table` answered no rows, where both real engines refuse the statement; so did `GROUP BY ROLLUP (a)`, `ROLLUP` parsing as an ordinary function call. Columns had been resolved before a statement runs since the join increment -- the entry above it in this file is about exactly that -- and **functions had not**, for no reason other than that nobody had looked.

**3. `SELECT upper()` crashed the interpreter.** Not a `DbError`: a Ziman runtime panic, `index 0 out of bounds (array length 0)`, from `eval_call` reading `args[0]` of an empty array. `length` and `substr` checked their arity; `lower`, `upper`, `trim` and `abs` did not. **SQL text took the process down**, which for an engine whose whole purpose is to run inside a test suite is the most serious of the three.

All of 2 and 3 come from one absence, and one list fixes both: `known_functions`, a name and an argument range per function, **read by validation before a statement runs and by evaluation as it runs**. An arity is now a fact about the function in one place instead of a check each implementation remembers or forgets, and the two messages are SQLite's to the letter -- `no such function: X` and `wrong number of arguments to function X()`, with the name echoed as written. A third message came out of it: an aggregate reached in scalar position is `misuse of aggregate function X()`, SQLite's wording again and the right complaint, since the function is not missing but in the wrong place. `eval_aggregate`'s own arity check went away in favour of the list, and so did two bespoke messages.

**The inventory also found two smaller mismatches, both now matching SQLite:** `coalesce(x)` with a single argument is refused (it needs two, and SQLite refuses it too -- this engine had read it as `x`), and the stale `unsupported function: ... the aggregates are the next increment` message, which had outlived the aggregates by several increments.

**And two SQLite quirks left deliberately unmodelled, listed in the header rather than left to be discovered:** SQLite accepts `count()` with no arguments as a synonym for `count(*)`, and has a *scalar* `max(a, b)`/`min(a, b)` beside the aggregates of the same name. PostgreSQL refuses the first and spells the second `greatest`/`least`. This engine refuses all three, so it is stricter than SQLite in two narrow places that the fixture does not assert agreement on.

**The numbers.** 141 read queries agree with real SQLite, up from 121; the 20 new ones are the unknown-function, wrong-arity and misused-aggregate cases, each checked on a query whose rows are empty so that the *refusal* is what is compared. Five `ALTER TABLE ... ADD <constraint>` forms are refused identically by both engines in a section of their own. 96 round-trip shapes settle in one pass, up from 89. The strictness fixture holds 15 divergences, up from 14.

One more thing the probe showed, recorded because it is a difference and not a defect: when a statement has two faults, the engines complain in their own order. `SELECT nosuchfunc(name) FROM visit`, where `visit` has no `name`, is `no such column: name` on SQLite and `no such function: nosuchfunc` here. Both refuse; which fault they look for first is their own business, and the fixture uses a column that exists so it is asserting the function rule rather than the order.


## Constraints stop being decoration (2026-10-25)

`NOT NULL`, `PRIMARY KEY`, `UNIQUE` and `CHECK` are **enforced**, at column level and at table level, on `INSERT` and on `UPDATE`, with SQLite's own messages. A new fixture runs 55 statements on `memdb` and on real SQLite and **all 55 agree on the first run**. `FOREIGN KEY` is modelled and not yet enforced; that is the next increment and it is named as such rather than implied.

**Until now every constraint was a run of text nobody looked inside.** That was honest while nothing enforced them -- the header said so in its own divergence list -- and useless the moment anything did, because half of what a test suite checks about a write is that the write is *refused*. So the parser's model changed first: `ConstraintRule` with `PrimaryKey([string])`, `Unique([string])`, `Check(Expr)`, `ForeignKey(ForeignKeyRule)` and `Opaque`, each carried by a `TableConstraint` that also keeps the source `text`. **An engine cannot enforce a string**, and that is the whole argument for modelling them.

Three decisions in that model are worth the space:

* **`format` writes the rule, not the text.** Echoing `text` would have been one line and would have made a constraint built in code format as nothing at all. `text` survives for the `Opaque` rule, which has nothing else, and for the messages an engine quotes.
* **A column-position `CHECK` or `REFERENCES` stays on the column.** Lifting them into the table's constraint list would have been simpler to enforce and would have rewritten every one of the sample application's migrations, which put every foreign key in column position. Two formatting functions is the price.
* **A *named* column constraint is still text.** `a INT CONSTRAINT k CHECK (..)` keeps its whole run verbatim and is not enforced, where the same constraint at table level is. Nothing is lost and nothing is pretended; the alternative was a pending-name variable that would drop the name on any shape it did not expect.

**The NULL rules are the reason this needs a cross-check rather than a careful reading**, because SQL gives NULL three different meanings inside four constraints:

| | |
|---|---|
| `NOT NULL` | refuses a NULL, and a column left out of an `INSERT` is a NULL unless it has a default |
| `UNIQUE`, `PRIMARY KEY` | **ignore** a key with a NULL in it -- two such keys never conflict, because uniqueness asks whether two keys are *equal* and a NULL is equal to nothing |
| `CHECK` | fails only when it is **false**; NULL is unknown and passes |

The second is the opposite of what `DISTINCT` and `GROUP BY` do with NULLs, where two NULLs *are* the same row. Both readings are correct and they sit in the same engine, one `values_same` apart -- which is why that function is shared by the three callers that want the grouping reading and skipped by the one that wants the uniqueness reading.

**And then there is the rowid, which is the quirk that would have made a false green out of the common case.** A single-column `INTEGER PRIMARY KEY` is SQLite's rowid under another name: a NULL in it, or leaving the column out of the `INSERT`, takes the next value instead of being refused. Measured rather than assumed, and the measurement mattered twice: the declared type must be **exactly** `INTEGER`, since `INT PRIMARY KEY` and `BIGINT PRIMARY KEY` are ordinary columns there and keep the NULL; and a *composite* primary key is not a rowid even when one of its columns is an `INTEGER`. A reading of the documentation would have given the first and probably missed the second.

Two new divergences between the readings, both measured on postgres:17, bringing the count to nine:

* **A `PRIMARY KEY` column may hold NULL in SQLite** -- genuinely, not only in the rowid case: `CREATE TABLE t (a TEXT PRIMARY KEY)` takes a NULL and takes a second one. The standard makes a key implicitly `NOT NULL` and PostgreSQL enforces that, so the strict reading refuses the write.
* **`ALTER TABLE ... ADD COLUMN u TEXT UNIQUE` is refused by SQLite** and accepted by PostgreSQL, because SQLite adds a column without rewriting the rows already there. The same reason makes SQLite refuse `ADD COLUMN x TEXT NOT NULL` outright (`Cannot add a NOT NULL column with default value NULL`) where PostgreSQL refuses it only when there are rows to falsify it -- so that one is a divergence *only* on an empty table, and is recorded as prose rather than asserted as a case.

**Enforcement happens before the new table is published**, so a refused statement leaves the database exactly as it was. That is free here, and it is free for the same reason transactions are: a `Database` is an immutable value, and the new one simply never gets published. An engine that mutated rows in place would need the undo log this one has never had.

One structural note. `Table` gained a `constraints` list holding **all** of them -- the table-level ones, the ones written in a column's position, and a synthesized `PrimaryKey`/`Unique` for every singly-flagged column -- so enforcement walks one list. Three places to remember is how a constraint gets enforced on `INSERT` and forgotten on `UPDATE`, and the four `Table` construction sites were found by the compiler the moment the field existed, which is the same mechanism that found the five `Expr` switches two entries above.

**The numbers.** 55 of 55 constraint statements agree with real SQLite, including the `Constraint` error *class* and not just its text -- a caller switches on that to tell "you may not" from "I could not read that". 141 read queries and 21 `RETURNING` statements still agree. 107 round-trip shapes settle in one pass, up from 96, the new ones being named constraints, composite keys, foreign keys with every action, and the interleavings of a column's flags with its constraints. The strictness fixture holds 17 divergences, up from 15.


## Referential integrity, and a pragma that makes a green suite mean nothing (2026-10-25)

Foreign keys are enforced: the child's side on `INSERT` and `UPDATE`, the parent's side on `DELETE` and on an `UPDATE` that moves a key, with `NO ACTION`, `RESTRICT`, `CASCADE`, `SET NULL` and `SET DEFAULT` all doing what they say. **51 statements run on `memdb` and on real SQLite and all 51 agree**, in a fixture of their own because referential integrity is about two tables rather than one row.

**The thing worth carrying away is about SQLite, not about this engine.** SQLite enforces foreign keys only when `PRAGMA foreign_keys = ON`, and it is **off by default**. This library's driver sets it at every connection (`src/native_sqlite.c:119`), which is the only reason the cross-check above is a comparison rather than a test against a no-op. An application whose suite runs against a plain `sqlite3` connection is testing no referential integrity whatsoever, and will meet it for the first time on PostgreSQL -- the same shape of surprise as the two-commit PostgreSQL outage recorded earlier in this file, and a reminder that "the suite is green on SQLite" is a claim about the pragmas as much as about the SQL.

**Four rules, each measured rather than reasoned about.**

* A child key with a **NULL** in it points at nothing and is never checked. That is the standard's `MATCH SIMPLE`, it is the default, and it means `NULL`'s third reading inside constraints: `NOT NULL` refuses it, `UNIQUE` ignores a key containing it, a `CHECK` passes on it, and a foreign key skips a key containing it.
* `SET NULL` and `SET DEFAULT` rewrite the child's key and then the child's **own** constraints are checked, which is how a `SET NULL` into a `NOT NULL` column becomes `NOT NULL constraint failed` rather than a NULL in a column that forbids them.
* **A cascade recurses and cannot loop.** No depth guard: a cascade only ever deletes, so each step strictly shrinks the database and a cycle between two tables runs out of rows rather than out of stack. The self-referencing case is in the fixture -- a five-node tree whose root deletion takes the whole chain.
* **The referenced columns must be a key of the parent**, a `PRIMARY KEY` or a `UNIQUE`, not merely columns that exist. Pointing at an unconstrained column is a mistake rather than a weaker reference: nothing stops the parent holding the value twice, so "the row it points at" would not be one row. SQLite refuses it when the statement runs (`foreign key mismatch - "child" referencing "parent"`), PostgreSQL when the table is created; the message here is SQLite's, and it covers a parent with no key at all.

**One bug, found by the fixture within a minute of being written.** `INSERT INTO tree (id, up) VALUES (1, NULL), (2, 1)` -- a multi-row insert whose second row points at its first -- was refused here and accepted by SQLite, because enforcement looked the parent up in the database *as it was before the statement*. For a self-referencing table the parent is the table being written, so it has to be read from the new state. One argument changed; the lesson is that "before or after the statement" is a question every check has to answer deliberately, and this one had answered it by accident.

**And one the fixture caused rather than caught**, which is worth recording because the diagnosis was the opposite of the symptom. Adding a deliberately malformed foreign key -- one pointing at a non-key column -- made SQLite refuse **every** `DELETE` on the table it named, including deletes with nothing to do with it: a parent's delete checks every key pointing at it, and the mismatch is reported before any action runs. Three fixture lines disagreed, and none of them was about the malformed key. The fix was to give the malformed shapes a parent of their own, and the finding is that a broken constraint anywhere in a schema can make unrelated statements fail on a real engine.

`DEFERRABLE` is read and ignored, so every constraint is checked as its statement runs. SQLite ignores it too unless `PRAGMA defer_foreign_keys` is set, so the two agree and the fixture says so; PostgreSQL honours it, and a test that needs a deferred check is asking for the real engine. That is the last of the constraint increment: `NOT NULL`, `PRIMARY KEY`, `UNIQUE`, `CHECK` and `FOREIGN KEY` are all enforced, and the module's divergence list no longer has a line saying constraints are decoration.


## Derived tables, and the line SQLite draws around them (2026-10-25)

`FROM (SELECT ..) AS x` -- a query in a `FROM`, run once and read like a table, with or without an alias, joined with others, nested, or holding a compound. **164 read queries agree with real SQLite, up from 141**, and 120 round-trip shapes settle in one pass, up from 107.

`TableRef` gained a `query: [SelectStmt]` -- empty for a named table, one element for a derived one -- which is the reshaping the module's own `#![experimental]` note predicted twice. An array rather than an `Option`, the way `CaseNode.fallback` is, so a `TableRef` need not name the type that contains it.

**The interesting question was what a derived table can see**, and it has a non-obvious answer that only measurement gives. A derived table is not `LATERAL`, so it cannot reference the `FROM` entry beside it:

```sql
SELECT x.reason FROM patient AS p, (SELECT reason FROM visit WHERE visit.patient_id = p.id) AS x
-- no such column: p.id, on SQLite and here
```

But it *is* inside whatever encloses the statement, so the same derived table inside a correlated subquery resolves outward and runs:

```sql
SELECT p.name FROM patient AS p
 WHERE p.id IN (SELECT y.patient_id FROM (SELECT patient_id FROM visit WHERE visit.patient_id = p.id) AS y)
-- runs on SQLite and here
```

Both were measured on real SQLite, and together they say exactly where the line is: a derived table is given the enclosing scopes and not the current one. **Which is what the existing `Ctx.outer` chain already does if the derived query is handed `outer` unchanged** -- the scope that would make the first query legal is the one being built and so is not in the chain yet. The two behaviours came out of one argument passed through, and I would not have trusted that without the pair of measurements.

**A comma in a `FROM` list turned out to be unread**, which the derived-table idiom exposed: `FROM t, (SELECT ..) AS x` is how people write it, and `FROM a, b` is Core SQL and the older spelling of `CROSS JOIN`. Nothing had noticed because the 39 real migrations and every fixture use an explicit `JOIN`. It is now read as the cross join it is -- four lines in `parse_joins`, rather than a second thing to model.

**Two new divergences, bringing the count to eleven:**

* **Duplicate column names in a derived table.** `SELECT x.id FROM (SELECT p.id, v.id FROM p JOIN v ..) AS x` is `column reference "id" is ambiguous` on PostgreSQL and **answers the first column** on SQLite. The rule that fits both is sharper than "count the matches": ambiguity is across *sources*, so two columns of one derived table are not ambiguous while the same two names across a join are, in either engine and either reading. `resolve_column` now counts distinct qualifiers rather than matches, with the one-source case ambiguous only in the strict reading.
* **A column alias list**, `(SELECT ..) AS x (a, b)`, is ordinary SQL in PostgreSQL -- which also accepts a list shorter than the query's and renames only that far -- and a syntax error in SQLite. The lax reading refuses it in SQLite's words; the strict one renames.

What is still not read is now a shorter list, and every item on it is a tree rather than a field: a parenthesised join expression, a parenthesised arm of a compound, `LATERAL`, and common table expressions. The flat `joins` and `set_ops` lists are what make the first two refusals rather than mistakes, and the round trip is what will make turning either into a tree safe.


## `sql` gets unit tests, and the unit tests find what the round trip cannot (2026-10-25)

Two things, and the order matters: `tests/stdlib/sql_test.zm` -- **57 tests asserting on the tree** -- and then an expression parser rewritten for speed, which those tests immediately caught breaking in two ways.

**The round-trip property cannot see a wrong tree that the formatter writes back consistently.** That is not a hypothetical: `ALTER TABLE t ADD CONSTRAINT k UNIQUE (a)` spent several increments being read as a column named `CONSTRAINT`, round-tripping perfectly the whole time, until an inventory happened to ask. 120 statement shapes settling and 164 cross-checked queries agreeing say nothing about a node's *contents*. So the new tests take the tree apart: a `shape` helper renders an expression as `Bin(=, Col(a), Lit(1))`, and each test names what a node has to be -- precedence, associativity, a parameter's number, an alias, a constraint's rule, a set operation's quantifier, a derived table's query.

**They found four defects within a minute of existing, and two were mine from the same sitting.**

* **`NOT a AND b` dropped the `AND b`.** The climbing loop returned straight out of its `NOT` branch instead of carrying on, so `NOT` swallowed the statement and the rest was "unexpected 'b' after the statement". **No fixture had an unbracketed `NOT x AND y` in it** -- the round-trip corpus and the cross-check between them had `NOT a = 1`, `NOT name LIKE 'A%'`, `NOT (age > 30 AND note IS NULL)`, and not the one shape the rewrite broke. A 120-shape corpus with a hole in it is still a corpus with a hole in it.
* **`a = 1 IS NULL` was refused.** The nine-function ladder read comparisons and predicates in one loop, so a predicate could follow a comparison; the climbing loop only re-checked predicates after an operator binding *tighter* than they do. A comparison sits **at** the predicate level, so the test is `>=`, not `>`.
* **`b INTEGER CONSTRAINT k CHECK (b > 0)` lost the name `k`**, because `parse_type_name` had no reason to stop at `CONSTRAINT` and read the type as `INTEGER CONSTRAINT k`. The `CHECK` was then modelled unnamed, so an engine quoted the expression where both real ones quote `k`. Again invisible to the round trip: both halves re-parse the same way.
* **`EXCLUDE USING gist (..)` was read as a column** named `EXCLUDE` of type `USING`. It is PostgreSQL's and is not modelled, but *preserving* it is the module's promise; `EXCLUDE` is now one of the words that begins a table constraint, so it comes back as written.

**Then the optimization, and the measurements that mattered are the ones that disproved me.** Parsing was 0.19 ms per statement, about 2 MB of SQL a second. Three guesses, all wrong:

* folding every token's text once at lex time instead of upper-casing it per comparison: **2%** (and lexing 5% slower);
* the twenty-word clause list and the other keyword scans as hash sets: **0%**;
* the lexer's per-character comparisons, as one `contains` and an inlined name loop: **0%**, so both were reverted.

`perf` then said where the time actually was: **19% in `strcmp` and about 20% in function-call machinery** (`call_function`, `env_define`, `env_lookup_resolved`, `overload_pick`, `ziman_push_frame`). Which pointed at two shapes rather than two lines:

* **The precedence ladder was nine functions, one per level.** Every atom of every expression -- every literal, every column name -- paid nine interpreted calls to reach `parse_primary`, and the comparison level asked its seven operators one `sees_punct` at a time. It is one climbing loop now, reading the operator from the token in one go and recursing at `prec + 1` for left associativity. Measured per select item: **13.3 microseconds down to 7.4**.
* **Every cursor question went through `peek`**, which copies a `Token` out of the array and constructs a fresh one at end of input, behind `at_end`, behind two more calls. `sees`, `accept` and the three keyword predicates now index the token array directly. A bare `SELECT 1` costs **45 microseconds down to 24**, which is the clause-keyword checks and almost nothing else.

**A/B interleaved, two binaries built from the same tree** -- because this file already records a case where non-interleaved sampling reported a real 15% cost as zero:

| | before | after |
|---|---|---|
| the corpus, 3,200 statements | 622 ms | 447 ms |
| a bare `SELECT 1` | 45 µs | 24 µs |
| a select list of 40 atoms | 582 µs | 313 µs |
| the application's 39 migration files, 20 passes | 392 ms | 326 ms |

So **1.4x over the corpus and 1.9x on the statements an engine runs most**, and only 1.2x on the migrations, which are many short DDL statements and therefore lexer-bound. Lexing is about 40% of what is left and is allocation-bound -- a substring, an upper-cased copy and a `Token` per name token -- which is a representation change rather than a tidy-up, so it is not done here. `format` was never the cost: it is ten times cheaper than `parse` and unchanged.

One tidy-up fell out that is worth more than its milliseconds: **the parser and the formatter now read one `precedence` function** instead of a table each. Two tables are two chances to disagree about what a query means, which is the shape of both precedence bugs this module has had.

`tests/bench/sql_parse.zm` is the standing measurement -- the corpus, a sweep over the number of expression atoms, and the real migration files -- and it exits 1 if parsing stops being linear in the atoms, which is what a ladder coming back would look like.


## `memdb` gets unit tests, and four quadratics come out of hiding (2026-10-25)

`tests/stdlib/memdb_test.zm` -- 22 tests -- and then the four operations they exposed as quadratic. Interleaved A/B of two binaries built from the same tree, 400 rows:

| | before | after | |
|---|---|---|---|
| `UPDATE` every row | 7,100 ms | 50 ms | **150x** |
| a self-join on `id` | 505 ms | 4 ms | **125x** |
| `WHERE id IN (SELECT ..)` | 397 ms | 12 ms | **33x** |
| `ORDER BY n, id` | 1,945 ms | 260 ms | **7.5x** |

**The cross-check fixtures could not have found any of this.** 164 queries agreeing with real SQLite, 57 constraint statements, 51 foreign-key statements -- all green, all the way through, because **a cross-check compares answers and not clocks**. That is the same blind spot the round-trip property has for a wrong tree, and the same answer: a different kind of test, not a bigger corpus of the same one.

**What each one was.**

* **A key's uniqueness was a scan of the whole table per written row.** A 400-row `UPDATE` with a `PRIMARY KEY` therefore did 160,000 key comparisons. It now builds a bucket index over the table once per statement and looks each written row up in it. The bucket name normalises numbers through `as_number`, so `Int(1)` and `Float(1.0)` land together as they must, and `values_same` still decides equality inside the bucket -- **the index chooses where to look, never what is equal**.
* **A join tried every pair.** 200 rows against 200 is 40,000 combined rows, each with a context built and a condition evaluated: measured at 13 microseconds a pair. Now an `ON` with an equality between a column of each side -- which is nearly every join anyone writes -- buckets the right side once and gives each left row only the rows that can match. The full condition is still evaluated on the survivors, so the index only ever *narrows*: a pair whose keys differ cannot satisfy `=`, and neither can a pair with a NULL. A join without such an equality still tries every pair, because nothing else can.
* **An uncorrelated subquery ran once per outer row.** `WHERE id IN (SELECT ..)` over 400 rows ran the inner query 400 times. A subquery that reads nothing from the row it is evaluated against answers the same thing every time, and `uncorrelated` is exactly `validate_select` **with no enclosing scopes**: if every column it names resolves inside it, nothing outside can change its answer. Such a subquery is now replaced, once per statement, by what it answers -- `IN (SELECT ..)` becomes an `IN` over literals, whose evaluation arm handles NULL identically; `EXISTS` becomes a boolean; a scalar subquery becomes its value. A correlated one is left exactly as it was and still runs per row, as it must in an engine with no indexes.
* **`ORDER BY` evaluated its keys inside the comparator**, so a term that is an expression was evaluated twice per comparison -- O(n log n) evaluations where O(n) will do. The keys are computed once per row now, which also let the comparator stop being able to fail, which in turn removed the failure-carrying cell that a lambda needs because it cannot capture a `var`.

**And the measurements that were wrong.** Two things I was sure about and that the clock disproved:

* folding every column name once per statement instead of per lookup -- `resolve_column` called `to_lower` on every candidate column of every reference of every row -- gained **nothing measurable** on the join it was supposed to fix. The join's cost was interpreted calls per pair, not allocations, which is the same answer `perf` gave for `sql`'s parser an hour earlier. The change stayed anyway, because it makes `Names` a thing that exists once rather than per row, which is what let the join's combined shape be folded once instead of per pair.
* a correlated `EXISTS` looked 60% slower afterwards, which sent me looking for a regression that was not there. Running that one query alone, interleaved, it is unchanged. **The bench runs everything in one process**, so an earlier line's allocations shape a later line's collector pauses; a 60% difference between two runs of the same binary is ordinary. The bench now says so in its header, and its verdict compares shapes across sizes rather than absolute times.

**One real bug, found by the subquery work and not by the clock.** Replacing an uncorrelated subquery with its answer requires knowing it is uncorrelated, and `validate_select` did not look inside a derived table's query -- so a correlation hidden in one (`IN (SELECT y.x FROM (SELECT x FROM v WHERE v.id = p.id) AS y)`) read as uncorrelated, was evaluated once with no outer row, and failed with "no such column: p.id". The derived query is now validated too, against the scopes enclosing the statement and not its siblings, which is the same rule it is *run* under. That also closed a hole the fixtures had: a typo inside a derived table used to be caught only when the derived table ran.

Also worth recording because it cost me ten minutes: a `python` replacement I did not verify silently did nothing -- the anchor said `q.items` where the code says `stmt.items` -- and I spent that time re-deriving a conclusion I had already reached. Every edit in that pass now asserts its anchor exists.

**What is still quadratic, stated rather than hidden.** A write is linear in the table, because a `Database` is an immutable value -- which is exactly what makes transactions free -- so seeding n rows one statement at a time is quadratic in principle; what was removed is the per-row *constants*, and 800 rows now seed in under two seconds where the shape alone predicts it. A long `IN` list is compared against per row. A correlated subquery runs per row. `memdb`'s header lists all of it under "What a statement costs", and `tests/bench/memdb_ops.zm` fails if a join or an uncorrelated subquery goes back to being quadratic.

The other 18 tests cover what no comparison can: that a refused statement leaves the database exactly as it was, that transactions nest, that `execute` reports what each statement kind touched, that a constraint violation arrives as `DbError.Constraint` and a syntax error as `DbError.Sql`, that the lazy `rows` form hands out what `query` does, and that each of the two readings refuses what the other runs.


## `datasource` tested with an engine that is not a database, and a wall clock taken out of the pool (2026-10-25)

`tests/stdlib/datasource_test.zm` -- 28 tests -- driven by a **driver made of counters**. This module's whole claim is that it names no engine: a `db.Driver` is a name, some URL prefixes, two questions about pooling and a function that opens a connection, and a `db.Conn` is four closures. So the sharpest test available is to hand it an engine that does not exist, which is also a demonstration of the claim.

**It is the only way to reach the paths that decide whether an application survives a database restart.** `tests/lang/db_pool_*.zm` drive a pool over real SQLite files, which cannot be made to drop a connection mid-statement, go stale while idle, or refuse to open on the third of four. A fake can, on command, and so the suite now covers:

* a pooled statement that fails with `DbError.Connection` -- the connection is closed and a fresh one opened in its place, and the next statement runs on the replacement;
* a statement that merely fails -- `DbError.Sql` says nothing about the connection, so nothing is replaced;
* `discard` replacing a connection where `release` keeps it;
* a replacement that **cannot** be opened, because the server is still down: the dead connection goes back rather than being lost, so the pool never shrinks, and the next borrow tries again;
* an `open` that fails partway through filling a pool: the connections already opened are closed, counted;
* the idle check in all three states -- probed and replaced, probed and kept, and never probed because the driver says a connection of its kind cannot go stale;
* a `with_transaction` that commits, one that rolls back, one whose connection dies (replaced) and five in a row over a pool of one (each must give the connection back or the second would block forever);
* a pooled `rows` iterator giving its connection back both when it is finished and when it is closed early.

**What the measurement said, and why it ends the optimisation question.** With a driver that does nothing, so the clock shows only this module:

| | overhead per statement |
|---|---|
| direct | 750 ns |
| pooled | 3,850 ns |
| `acquire` + `release` | 1,550 ns |
| `with_transaction` | 4,800 ns |

A PostgreSQL round trip is 300,000 to 3,000,000 ns and a SQLite file statement 5,000 to 50,000. **A pooled borrow is a fraction of a percent of a network engine**, so there is no optimisation here worth a correctness risk -- and the point of measuring was to make that a decision rather than an assumption. `tests/bench/datasource_overhead.zm` keeps the number honest with a ceiling an order of magnitude above it, which only a borrow path that started doing real work could cross.

**One change was worth making, and it is a correctness fix that happens to be faster.** A pooled connection was stamped with `now_raw()[0]` -- Unix wall-clock seconds -- when it was parked, and the idle check compared that against the wall clock again. A clock step moves both: an NTP correction forward makes every idle connection look stale and get probed, and one backward makes them all look fresh for as long as the step was, so **the health check this exists for would quietly stop happening** on a machine whose clock was corrected. It reads `monotonic_millis` now, which cannot be moved and allocates nothing where `now_raw` built a two-element array per park and per borrow. `check_after` is converted to milliseconds once when the pool is made; the API still takes seconds, and zero or less still turns the check off, which `tests/integration/pg/pg_pool_health.zm` relies on. Interleaved A/B: a pooled statement 5,050 to 4,800 ns, and `acquire` + `release` 2,450 to 1,950.

**And a measurement I nearly reported as a regression.** `with_transaction` looked 28% slower afterwards, consistently, across three interleaved passes. Measured alone with ten times the rounds it is identical -- 143/137/144 against 141/143/134 ms. It had been the last line of a process that had already run 80,000 statements, and the collector's state at that point is what differed. That is the caveat I had written into `tests/bench/memdb_ops.zm` an hour earlier, and the thing worth recording is that **the habit paid: the check cost two minutes and would have cost an afternoon of looking for a cause that was not there.**

One testability finding, left as it is on purpose. `check_after_seconds` of zero means "do not test", so the smallest reachable setting is one second and the one test that exercises a stale connection has to wait for it. Redefining zero as "test every time" would make the suite a second faster and would silently turn the check *on* for `tests/integration/pg/pg_pool_health.zm`, which passes zero to turn it off. A second of waiting is the cheaper of the two, and three pools are set up before the single sleep so it is paid once.


## `db`: a malformed row stops crashing, and an optimisation measured and declined (2026-10-26)

`tests/stdlib/db_test.zm` -- 23 tests over the part every row read in the application goes through: `row_get`, the `sql_as_*` family, `sql_lift`, `db_error_message`, and the URL helpers the 30-URL fixture leaves out.

**One defect, and it is the same shape as `upper()` crashing `memdb` yesterday.** A `Row` whose `columns` has more names than `values` has values ended the process:

```
ziman: runtime error at 150:30: index 1 out of bounds (array length 1)
    at db::row_get (15:18)
```

Not a `DbError` a caller could report -- a panic. A `Row` is built by a driver out of what a server sent, so the way to get one is a protocol desync: a PostgreSQL `DataRow` carrying fewer fields than its `RowDescription` promised. The check is on the hit rather than at the top of the scan, so the common path costs nothing for it, and the message names both counts. **Finding this took a probe that fed the function something no happy path produces**, which is the only way these are ever found: 20 tests written from the documented behaviour all passed first time and found nothing.

**What is not tested here, stated rather than left as a gap.** The other half of this module is the cursor machinery `pg` and `mysql` share -- `open_rows`, the page buffer, the request protocol with the thread that owns a socket. Its `Request` and `Page` types are `internal` **on purpose**: they are a wire protocol between a connection and its owning thread, not something a caller constructs. So a test outside the package cannot play the owning thread, and the attempt fails at the type system:

```
struct 'Page' in module 'db' is internal to its own package
```

That is the privacy working, and the price of it is that this code is covered only by `make test-pg` and `make test-mysql`, which need containers. Worth writing down as the deliberate trade it is, so the gap is not mistaken for an oversight.

**And an optimisation measured, then declined.** `row_get` scans a row's names, so reading every column of a row is quadratic in its width. Measured on a 12-column row: 290 ns for the first column, 790 for the last, **8 microseconds to read the whole row** -- so a thousand rows of twelve columns spend about 8 ms here, against 5 to 20 ms on the wire for the query itself. A third of the client's own work, which is worth knowing.

Two candidates, both measured:

* **Reading the length once** instead of per iteration: 17-20% off a scan that walks twelve columns or more, 3-7% *added* to one that stops at the first or second, and **no measurable difference** to reading a whole 12-column row, which is what a `from_row` does. The cost is the call and the string comparisons, not the loop condition. Reverted, with the numbers in the function's comment so the next person does not redo it.
* **An index built once per result and shared by its rows**, the way `columns` already is: 215 ns a lookup against 790, costing 3,900 ns to build for twelve columns, so it pays for itself after five lookups in a result set and saves about 9 ms on a thousand-row read. **Not done**, because the callers that would benefit are the `from_row` functions `#[derive(Row)]` emits, so adopting it means changing what the compiler generates and every generated signature. That is a deliberate increment with these numbers in hand, not a change to make in passing -- and the numbers are now in the comment that would start it.

So the module is unchanged except for the crash, which is the right outcome: this layer is types and converters, and the only thing in it that costs anything is a scan whose fix lives somewhere else.


## `#[derive(Entity)]`, and the table name it refuses to guess (2026-10-26)

The first step toward an entity query language, and valuable on its own: `#[derive(Entity)]` writes `<name>_columns` -- the struct's field names in `to_params` order. The sample application's eighteen entities drop their hand-written `pub val columns = [...]` arrays and take the list from the struct instead.

**That array was a real drift risk, recorded in the code's own comments before this.** `entity/patient.zm` notes the profile growing "from 8 columns to 24 in one commit", and `service/crud.zm`'s E9 note is about a hand-numbered `INSERT` that "would have gone on compiling while passing 24 parameters to 8 holes". The list and `to_params` had to agree in *order*, and nothing checked that they did. Now the struct is the only place either is written.

Before changing them I checked all eighteen against their structs, because replacing a list that had already drifted would have changed behaviour rather than preserving it: **every one matched, field for field, in order.** A good outcome to have verified rather than assumed.

**The table name is not derived, and that is the interesting decision.** The obvious convention -- snake_case the struct, add an `s` -- is wrong for three of the eighteen:

| struct | table |
|---|---|
| `FamilyHistory` | `family_history` |
| `MedicalHistory` | `medical_histories` |
| `UserNotificationPreference` | `user_notification_prefs` |

A wrong guess there generates SQL against a table that does not exist, and the failure arrives at runtime, in whichever request first touches that entity. So the derive emits only what the compiler knows for certain, and the table stays a hand-written `val` beside the struct -- one line, and one that cannot be silently wrong, because the database refuses it. (`#[test(cases: ident)]` shows attributes take identifiers rather than string literals, so `#[entity(table: "patients")]` would have needed a lexer change as well; the evidence made that moot.)

`Entity` **requires `Row`**: an entity is something read back out of a row, and `<name>_from_row` is what reads it, so deriving the column list alone leaves half a metamodel -- a query layer able to name the columns and unable to build the value. The error says so by name.

**Two compiler findings came out of writing it.**

* `parse_and_splice` carried only the *functions* out of a derive's generated source. A `pub val` parsed cleanly and then vanished, and the first reference to it failed with "use of undefined variable" -- with a position pointing at a comment in a different file, because the spliced declaration's tokens all carry the deriving struct's line. Every derive until now emitted functions only, so nothing had noticed.
* Prepending those vals rather than appending them matters. Module-level vals initialise in list order, so `pub val columns = patient_columns` -- written beside the struct, above the derive's output -- was evaluated before a val pushed onto the end of the list. A derived val's initialiser is a literal that references no other val, so putting them all first is safe and makes the derived name available wherever the hand-written vals happen to sit.

This is increment 1 of four. The rest, decided but not built: `lib/db/eql.zm` translating JPQL-shaped text into a `sql.SelectStmt` (entities resolved through a registry, the way `datasource` resolves drivers, so the module names no entity); execution through `datasource` with the translator numbering the parameters, which is what removes the hand-numbered `$2, $3, ...` in `repository/paging.zm`; and adopting it in one repository rather than eighteen, so it earns the rest.


## `eql`: an entity query language that is a translation, not an engine (2026-10-26)

`lib/db/eql.zm` -- JPQL's shape over a Ziman struct. A query names entities and their fields; the answer is the SQL those names stand for, plus the `sql.Statement` it parsed into and the parameter names in the order the SQL numbers them.

```
SELECT p FROM Patient p WHERE p.last_name = :name ORDER BY p.created_at DESC
-> SELECT p.id AS id, p.user_id AS user_id, .. FROM patients AS p WHERE p.last_name = $1 ORDER BY p.created_at DESC
   bindings: ["name"]
```

**It is a translator and nothing else.** Everything downstream stays what it was: `datasource` runs the text, `memdb` can run it in a test, `sql.format` decides how it is spelled. That is what made increment 2 a day's work rather than a project -- the tree, the formatter, the dialect knowledge and the test harness were all already there.

**The rewrite happens on tokens, and that is the whole design.** `sql.tokenize` is now public, because reading SQL-shaped text is not only `sql`'s job, and a token run is the *only* level at which this is safe: a `:` inside a quoted string or a `--` comment is text and not a parameter, and only the lexer knows which is which. Two of the tests exist for exactly that, and a text-level substitution would fail both. The lexer also gained `:` as a punctuation token -- SQL has no bare colon, so for SQL the only change is that one is now an unexpected *token* rather than an unexpected character, which is a better error.

The rewritten run is handed back to **`sql.parse` before it is answered**, so a translation that produced something the SQL grammar cannot read is reported at translation, with the SQL it became, instead of by a database. And because the expression grammar does the rest, `CASE`, `BETWEEN`, `IN`, `CAST`, function calls, subqueries and set operations all work here without this module knowing they exist -- there is a test asserting each, and the module contains no code for any of them.

**What it refuses is the point.** Every one of these, written as SQL by hand, compiles and deploys and fails on a request:

| query | |
|---|---|
| `SELECT p.last_nmae FROM Patient p` | `entity Patient has no field 'last_nmae' (it has: id, user_id, first_name, last_name, created_at)` |
| `SELECT p.reason FROM Patient p` | the field exists -- on the other entity |
| `SELECT q.id FROM Patient p` | `nothing in this query binds the alias 'q' (it binds: p)` |
| `SELECT p FROM Patient` | `entity Patient needs an alias` |
| `SELECT x.id FROM Appointment x` | `no registered entity is called 'Appointment' (registered: Patient, Visit)` |
| `FROM Patient p, Visit p` | `the alias 'p' is already bound to Patient` |
| `WHERE p.id = ?` or `= $1` | `an entity query's parameters are named, so write ':name'` |

**And `arrange` is what removes the hand-numbering.** A caller binds by name, in any order, and the translator lays the values out in the order the SQL ended up numbering them. A name the query does not use is an error, and so is a parameter nothing was bound to -- where `repository/paging.zm`'s hand-built ` AND type IN ($2, $3, ...)` and its separate `params_of` would simply have shifted every later parameter by one. `:id` written twice is **one** parameter bound once, which the hand-written form cannot express at all.

Entities are resolved through a **registry** -- `eql.registry([patient.meta, visit.meta])` -- the same shape `datasource` uses for drivers and for the same reason: this module names no entity, so an application that has three hands over three. The metamodel is `#[derive(Entity)]`'s column list plus the hand-written table name, so what a query may name and what `<name>_from_row` reads are the same list by construction.

One bug, found by the tests on their first run: `FROM Patient p, Visit v` left the second entity's *name* in the SQL, because the flag that says "an entity name comes next" was consumed by the first entity and never set again. A comma inside a `FROM` list means another entity; a comma anywhere else means another select item; the translator now tracks which it is in.

**What it does not read, and the honest cost.** No associations -- the entities declare none, so there are no path expressions across entities and no fetch joins; a join names the second entity and its condition, as the repositories already do. No `SELECT new Dto(..)`: a projection answers rows. No bulk `UPDATE`/`DELETE`: writes stay SQL, where an explicit statement is worth more than brevity. Those are increments, not oversights, and the first of them is the one that brings N+1 with it.

Increment 3 is execution -- `eql.list`/`one`/`rows` over `datasource`, cross-checked against `memdb` and real SQLite -- and increment 4 is adopting it in **one** repository rather than eighteen, so it earns the rest.


## `eql` runs, and the cost that decided its shape (2026-10-26)

`eql.list`, `eql.one`, `eql.rows` and `eql.count` over `datasource`, with 13 unit tests against `memdb` and a 17-query cross-check that runs each translation on **both** memdb and real SQLite.

**The API shape came out of a measurement, not a preference.** Translating a query costs 140 to 300 microseconds -- tokenise, rewrite, parse, format -- against 300 to 3,000 for a PostgreSQL round trip. Translating per call would therefore add between a tenth and the whole of a fast query's cost, for an answer that never changes. So the executing functions take a **`Translated`**, not a query string, and a repository translates once in a module-level `val`:

```ziman
val by_last_name = unwrap(eql.translate(reg, "SELECT p FROM Patient p WHERE p.last_name = :name"))

pub fn find_by_last_name(conn: datasource.Connection, name: string) -> Result<[Patient], DbError> {
    return eql.list<Patient>(conn, by_last_name, [eql.bind("name", SqlValue.Text(name))], patient_from_row)
}
```

A one-off is still one line -- `eql.list(conn, try eql.translate(reg, q), binds, from_row)` -- it just says what it is paying for. Had the functions taken a string, the fast path would have been the one nobody writes.

**`from_row` is passed at the call site**, and that is forced rather than chosen: a `Registry` holding `fn(Row) -> Result<E, DbError>` would be typed over `E`, and a registry that can hold only one entity type is not a registry. The metamodel stays untyped and the call site, where the type is known anyway, supplies the reader.

**The cross-check is what makes any of this a claim about SQL.** The unit tests and the translator read the same translator, so between them they cannot tell a subtly wrong translation from a right one -- a column aliased where it should not be, a parameter numbered out of order, a join that loses rows. Running each translation on two engines can: **17 of 17 agree**, over entity expansion, projections, counts, aggregates with `GROUP BY` and `HAVING`, inner and left joins, a comma join, `IN (SELECT ..)`, `EXISTS`, `LIKE`, `UNION`, `CASE`, and `LIMIT`/`OFFSET`. The fixture also prints the SQL each query becomes, which is the part a reviewer should actually read: a change there is a change in meaning.

Two small things the tests decided:

* **`count` refuses a query that answers more than one row or more than one column.** `SELECT count(*) AS n FROM ..` is the commonest projection there is and reading it by hand is four lines of row and converter every time -- but a helper that quietly read the first of several numbers would be worse than no helper, so it names what the query answered instead.
* **A missing binding names what *was* bound.** The first version said `the query names :name and nothing was bound to it`, which is true and unhelpful when the cause is `eql.bind("nmae", ..)` two lines away. It now ends `(bound: nmae)`, and the two spellings side by side diagnose it at a glance. The test that found this was asserting the wrong message, which is its own small lesson: the test was right that something was wrong and wrong about what.

One limit worth stating because it will surprise someone: a row answering **two** entities (`SELECT p, v FROM ..`) has two columns called `id`, and `row_get` takes the first -- so one `from_row` reads the first entity and the second is unreachable through it. Select one entity at a time, or use `rows` and read the tuple. JPQL answers an array of objects there; doing the same here needs a tuple type this layer does not have, which makes it an increment rather than a bug.

What is left of the plan is increment 4: adopting this in **one** repository -- `health_event`, whose type filter and paging are the hand-numbered parameters that motivated the whole thing -- rather than in eighteen, so it earns the rest.
