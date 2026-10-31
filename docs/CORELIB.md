# Ziman core module reference

Auto-generated from each `lib/core/*.zm` or `lib/db/*.zm` file's own `pub` declarations by
`tools/gen_corelib_reference.sh` -- **don't hand-edit the signature
blocks below**; regenerate with `make corelib-reference` after changing
any core module (adding, removing, or renaming a `pub struct`/`pub enum`/
`pub fn`/`pub native fn`/`pub val`). This is a signature lookup, not an
explanation -- see the manual in docs/lib/ for how to use each module, docs/syntax/ for full semantics and docs/design/journal/
for why each module is shaped the way it is.

`use <module>` resolves any of these with no local file needed (see
docs/syntax/'s "Modules" -- "Bundled core module"); a per-name import
(`use <module>.Name`) binds one item bare instead of qualified.

One exception, not listed below: `lib/prelude/` -- unlike every
module here, it's never `use`-able at all (see its own header comment
and docs/design/journal/'s "Prelude" entry), since everything it declares
(`Option<T>`, plus a handful of native functions) is already bare and
unqualified in every program, the same way `print`/`str`/`parse_int`
are. See docs/syntax/'s "Modules" section for what it contains.

## `bits`

```
pub native fn and(a: byte, b: byte) -> byte

pub native fn or(a: byte, b: byte) -> byte

pub native fn xor(a: byte, b: byte) -> byte

pub native fn not(a: byte) -> byte

pub native fn and(a: int, b: int) -> int

pub native fn or(a: int, b: int) -> int

pub native fn xor(a: int, b: int) -> int

pub native fn not(a: int) -> int

pub native fn and(a: bigint, b: bigint) -> bigint

pub native fn or(a: bigint, b: bigint) -> bigint

pub native fn xor(a: bigint, b: bigint) -> bigint

pub native fn not(a: bigint) -> bigint

pub native fn shift_left(a: byte, n: int) -> byte

pub native fn shift_right(a: byte, n: int) -> byte

pub native fn shift_left(a: int, n: int) -> int

pub native fn shift_right(a: int, n: int) -> int

pub native fn shift_left(a: bigint, n: int) -> bigint

pub native fn shift_right(a: bigint, n: int) -> bigint

pub native fn popcount(a: byte) -> int

pub native fn popcount(a: int) -> int

pub native fn popcount(a: bigint) -> int

pub native fn bit_length(a: bigint) -> int

pub struct BitSet {
    bits: int
}

pub fn bitset_new() -> BitSet

pub fn bitset_of_int(x: int) -> BitSet

pub fn bitset_to_int(bs: BitSet) -> int

pub fn set(bs: BitSet, i: int) -> BitSet

pub fn remove(bs: BitSet, i: int) -> BitSet

pub fn flip(bs: BitSet, i: int) -> BitSet

pub fn get(bs: BitSet, i: int) -> bool

pub fn intersect(sets: variadic BitSet) -> BitSet

pub fn union(sets: variadic BitSet) -> BitSet

pub fn difference(a: BitSet, b: BitSet) -> BitSet

pub fn is_subset(a: BitSet, b: BitSet) -> bool

pub fn xor(a: BitSet, b: BitSet) -> BitSet

pub fn not(bs: BitSet) -> BitSet

pub fn is_empty(bs: BitSet) -> bool

pub fn len(bs: BitSet) -> int

pub fn bit_length(bs: BitSet) -> int

pub fn to_array(bs: BitSet) -> [int]

pub fn format(bs: BitSet) -> string

pub struct BigBitSet {
    bits: bigint
}

pub fn big_bitset_new() -> BigBitSet

pub fn big_bitset_of_bigint(x: bigint) -> BigBitSet

pub fn big_bitset_to_bigint(bs: BigBitSet) -> bigint

pub fn set(bs: BigBitSet, i: int) -> BigBitSet

pub fn remove(bs: BigBitSet, i: int) -> BigBitSet

pub fn flip(bs: BigBitSet, i: int) -> BigBitSet

pub fn get(bs: BigBitSet, i: int) -> bool

pub fn intersect(sets: variadic BigBitSet) -> BigBitSet

pub fn union(sets: variadic BigBitSet) -> BigBitSet

pub fn difference(a: BigBitSet, b: BigBitSet) -> BigBitSet

pub fn is_subset(a: BigBitSet, b: BigBitSet) -> bool

pub fn xor(a: BigBitSet, b: BigBitSet) -> BigBitSet

pub fn is_empty(bs: BigBitSet) -> bool

pub fn len(bs: BigBitSet) -> int

pub fn bit_length(bs: BigBitSet) -> int

pub fn to_array(bs: BigBitSet) -> [int]

pub fn format(bs: BigBitSet) -> string

```

## `clock`

One native function reading the system clock (`clock_gettime`) -- Ziman's other source of genuine non-determinism.

```
pub native fn now_raw() -> [int]

pub native fn now_iso() -> string

pub native fn now_http_date() -> string

pub native fn monotonic_millis() -> int

pub native fn sleep_millis(ms: int)

```

## `compress`

```
pub fn gzip(data: [byte]) -> [byte]

pub fn deflate(data: [byte]) -> [byte]

pub fn gunzip(data: [byte], max_size: int) -> Result<[byte], string>

pub fn inflate(data: [byte], max_size: int) -> Result<[byte], string>

pub fn decompress(data: [byte], max_size: int) -> Result<[byte], string>

pub fn inflate_raw(data: [byte], max_size: int) -> Result<[byte], string>

```

## `crypto`

Hashes (SHA-1/256/384/512), HMAC, PBKDF2, password hashing (bcrypt, Argon2id), a constant-time byte comparison, base64 and base64url, on OpenSSL. No ciphers or public-key operations.

```
pub native fn sha1(data: [byte]) -> [byte]

pub native fn sha256(data: [byte]) -> [byte]

pub native fn sha384(data: [byte]) -> [byte]

pub native fn sha512(data: [byte]) -> [byte]

pub native fn hmac_sha1(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha256(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha384(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha512(key: [byte], data: [byte]) -> [byte]

pub native fn pbkdf2_hmac_sha256(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]

pub native fn pbkdf2_hmac_sha512(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]

pub fn bcrypt_hash(password: string, cost: Option<int> = 10) -> Result<string, string>

pub native fn bcrypt_verify(password: string, hash: string) -> bool

pub native fn argon2id_raw(

pub fn argon2id_hash(

pub fn argon2id_verify(password: string, encoded: string) -> bool

pub fn password_hash(password: string) -> Result<string, string>

pub fn password_verify(password: string, stored: string) -> bool

pub fn password_needs_rehash(stored: string) -> bool

pub native fn aes_gcm_encrypt(key: [byte], plaintext: [byte]) -> Result<[byte], string>

pub native fn aes_gcm_decrypt(key: [byte], blob: [byte]) -> Result<[byte], string>

pub native fn ecdsa_p256_generate() -> Result<string, string>

pub native fn ecdsa_p256_public_pem(private_key_pem: string) -> Result<string, string>

pub native fn ecdsa_p256_public_point(key_pem: string) -> Result<[byte], string>

pub native fn ecdsa_p256_sign(private_key_pem: string, data: [byte]) -> Result<[byte], string>

pub native fn ecdsa_p256_verify(public_key_pem: string, data: [byte], signature: [byte]) -> bool

pub native fn bytes_xor(a: [byte], b: [byte]) -> [byte]

pub native fn bytes_equal(a: [byte], b: [byte]) -> bool

pub fn base64_encode(data: [byte]) -> string

pub fn base64_decode(s: string) -> Result<[byte], string>

pub fn base64url_encode(data: [byte]) -> string

pub fn base64url_decode(s: string) -> Result<[byte], string>

```

## `csv`

RFC 4180 CSV as a `Json` array of objects keyed by the header row (typed or string cells; TSV via the `_with` variants); a table writer that flattens one level.

```
pub fn parse_rows(text: string) -> Result<[[string]], string>

pub fn parse_rows(text: string, delimiter: string) -> Result<[[string]], string>

pub fn parse(text: string) -> Result<Json, string>

pub fn parse(text: string, delimiter: string) -> Result<Json, string>

pub fn parse_typed(text: string) -> Result<Json, string>

pub fn parse_typed(text: string, delimiter: string) -> Result<Json, string>

pub fn parse_columns(text: string, columns: [string]) -> Result<Json, string>

pub fn parse_columns(text: string, columns: [string], delimiter: string) -> Result<Json, string>

pub fn parse_columns_typed(text: string, columns: [string]) -> Result<Json, string>

pub fn parse_columns_typed(text: string, columns: [string], delimiter: string) -> Result<Json, string>

pub fn records(path: string) -> Result<Iter<Result<Json, string>>, string>

pub fn records(path: string, delimiter: string) -> Result<Iter<Result<Json, string>>, string>

pub fn records_typed(path: string) -> Result<Iter<Result<Json, string>>, string>

pub fn records_typed(path: string, delimiter: string) -> Result<Iter<Result<Json, string>>, string>

pub fn format(rows: [[string]]) -> string

pub fn format(rows: [[string]], delimiter: string) -> Result<string, string>

pub fn format(rows: Json) -> Result<string, string>

pub fn format(rows: Json, sep: string) -> Result<string, string>

```

## `derive_json`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn from_json(d: meta.Decl, members: [meta.Field]) -> Result<string, string>

pub fn from_json_all(d: meta.Decl, members: [meta.Field]) -> Result<string, string>

pub fn enum_to_json(d: meta.Decl, variants: [meta.Variant]) -> Result<string, string>

pub fn enum_from_json(d: meta.Decl, variants: [meta.Variant]) -> Result<string, string>

pub fn generate(d: meta.Decl) -> Result<string, string>

pub fn to_json(d: meta.Decl, members: [meta.Field]) -> Result<string, string>

```

## `derive_ord`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_properties`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_reflect`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_validate`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn attributes() -> [meta.AttrSpec]

pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `encoding`

```
pub fn base2_encode(data: [byte]) -> string

pub fn base2_decode(text: string) -> Result<[byte], string>

pub fn base8_encode(data: [byte]) -> string

pub fn base8_decode(text: string) -> Result<[byte], string>

pub fn base16_encode(data: [byte]) -> string

pub fn base16_decode(text: string) -> Result<[byte], string>

pub fn base32_encode(data: [byte]) -> string

pub fn base32_decode(text: string) -> Result<[byte], string>

pub native fn base64_encode(data: [byte]) -> string

pub native fn base64_decode(s: string) -> Result<[byte], string>

pub native fn base64url_encode(data: [byte]) -> string

pub native fn base64url_decode(s: string) -> Result<[byte], string>

```

## `expr`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub struct MemberNode {
    target: Expr
    name: string
    // `?.` rather than `.`: a `null` target answers `null` instead of failing. §8 of the design
    // is why absence is an error by default and this is how you say otherwise.
    safe: bool
}

pub struct IndexNode {
    target: Expr
    key: Expr
}

pub struct UnaryNode {
    op: string
    operand: Expr
}

pub struct BinaryNode {
    op: string
    left: Expr
    right: Expr
}

pub struct TernaryNode {
    test: Expr
    then: Expr
    otherwise: Expr
}

pub struct CallNode {
    name: string
    args: [Expr]
}

pub enum Expr {
    Lit(Json)
    // A bare name, resolved against the context object's keys.
    Root(string)
    Member(MemberNode)
    Index(IndexNode)
    Unary(UnaryNode)
    Binary(BinaryNode)
    Ternary(TernaryNode)
    Call(CallNode)
}

pub fn parse(source: string) -> Result<Expr, string>

pub fn must_parse(source: string) -> Expr

pub fn eval(e: Expr, ctx: Json) -> Result<Json, string>

pub fn eval_bool(e: Expr, ctx: Json) -> Result<bool, string>

pub fn eval_string(e: Expr, ctx: Json) -> Result<string, string>

pub fn names(e: Expr) -> [string]

pub fn kind_of(v: Json) -> string

```

## `fs`

```
pub enum FsError {
    NotFound(string)
    Denied(string)
    IsDirectory(string)
    NotDirectory(string)
    Exists(string)
    Other(string)
}

pub fn fs_error_message(e: FsError) -> string

pub fn read_text(path: string) -> Result<string, FsError>

pub fn lines(path: string) -> Result<Iter<Result<string, string>>, FsError>

pub fn chunks(path: string, size: int) -> Result<Iter<Result<[byte], string>>, FsError>

pub fn walk(dir: string) -> Result<Iter<Result<string, string>>, FsError>

pub fn write_text(path: string, text: string) -> Result<int, FsError>

pub fn append_text(path: string, text: string) -> Result<int, FsError>

pub fn write_text_atomic(path: string, text: string) -> Result<int, FsError>

pub fn read_bytes(path: string) -> Result<[byte], FsError>

pub fn write_bytes(path: string, data: [byte]) -> Result<int, FsError>

pub fn append_bytes(path: string, data: [byte]) -> Result<int, FsError>

pub native fn exists(path: string) -> bool

pub native fn is_dir(path: string) -> bool

pub native fn is_file(path: string) -> bool

pub fn size(path: string) -> Result<int, FsError>

pub fn modified_seconds(path: string) -> Result<int, FsError>

pub fn list_dir(path: string) -> Result<[string], FsError>

pub fn resolve(name: string) -> Option<string>

pub fn remove(path: string) -> Result<unit, FsError>

pub fn remove_all(path: string) -> Result<unit, FsError>

pub fn make_dir(path: string) -> Result<unit, FsError>

pub fn make_dirs(path: string) -> Result<unit, FsError>

pub fn rename(from: string, to: string) -> Result<unit, FsError>

pub fn copy_file(from: string, to: string) -> Result<int, FsError>

pub fn temp_dir() -> string

pub fn path_join(pieces: variadic string) -> string

pub fn file_name(path: string) -> string

pub fn parent_dir(path: string) -> string

pub fn extension(path: string) -> string

```

## `json`

A JSON parser/serializer (`Json`, a genuinely recursive enum), written in pure Ziman -- no native code needed. `\uXXXX` unicode escapes are read; `==`/`!=` on `Json` is the one known gap.

```
pub enum Number {
    Int(int)
    Float(float)
    Bigint(bigint)
    Decimal(decimal)
}

pub enum Json {
    JNull
    JBool(bool)
    JNumber(Number)
    JString(string)
    JArray([Json])
    JObject(Map<string, Json>)
}

pub fn parse(s: string) -> Result<Json, string>

pub fn parse_strict(s: string) -> Result<Json, string>

pub fn parse(data: [byte]) -> Result<Json, string>

pub fn parse_strict(data: [byte]) -> Result<Json, string>

pub fn parse_exact(s: string) -> Result<Json, string>

pub fn parse_exact(data: [byte]) -> Result<Json, string>

pub fn to_bytes(j: Json) -> [byte]

pub fn format(j: Json) -> string

pub fn get(j: Json, key: string) -> Option<Json>

pub fn get_path(j: Json, path: variadic string) -> Option<Json>

pub fn require(j: Json, key: string) -> Result<Json, string>

pub fn require_string(j: Json, key: string) -> Result<string, string>

pub fn require_int(j: Json, key: string) -> Result<int, string>

pub fn require_float(j: Json, key: string) -> Result<float, string>

pub fn require_bigint(j: Json, key: string) -> Result<bigint, string>

pub fn require_decimal(j: Json, key: string) -> Result<decimal, string>

pub fn require_bool(j: Json, key: string) -> Result<bool, string>

pub fn to_map<V>(j: Json, decode: fn(Json) -> Result<V, string>) -> Result<Map<string, V>, string>

pub fn from_map<V>(m: Map<string, V>, encode: fn(V) -> Json) -> Json

pub fn to_string_map(j: Json) -> Result<Map<string, string>, string>

pub fn snake_keys(j: Json) -> Result<Json, string>

pub fn format(j: Json, indent: int) -> string

pub fn json_to_json(j: Json) -> Json

pub fn json_from_json(j: Json) -> Result<Json, string>

pub fn int_to_json(n: int) -> Json

pub fn number_of(j: Json) -> Option<Number>

pub fn int_from_json(j: Json) -> Result<int, string>

pub fn bigint_to_json(n: bigint) -> Json

pub fn bigint_from_json(j: Json) -> Result<bigint, string>

pub fn float_to_json(f: float) -> Json

pub fn float_from_json(j: Json) -> Result<float, string>

pub fn decimal_to_json(d: decimal) -> Json

pub fn decimal_from_json(j: Json) -> Result<decimal, string>

pub fn bool_to_json(b: bool) -> Json

pub fn bool_from_json(j: Json) -> Result<bool, string>

pub fn string_to_json(s: string) -> Json

pub fn string_from_json(j: Json) -> Result<string, string>

```

## `jwt`

JSON Web Tokens signed with a shared secret (HS256/384/512): sign, and verify with expiry checks.

```
pub enum JwtError {
    Malformed(string)
    UnsupportedAlgorithm(string)
    BadSignature
    Expired
    NotYetValid
    MissingClaim(string)
    WrongIssuer
    WrongAudience
}

pub fn jwt_error_message(e: JwtError) -> string

pub fn algorithm_for_key(key: [byte]) -> string

pub fn claims(subject: string, expires_in_seconds: int) -> Json

pub fn with_claim(claims: Json, name: string, value: Json) -> Json

pub fn sign(claims: Json, key: [byte]) -> Result<string, string>

pub fn sign(claims: Json, key: [byte], algorithm: string) -> Result<string, string>

pub fn sign(claims: Json, private_key_pem: string) -> Result<string, string>

pub fn sign(claims: Json, private_key_pem: string, algorithm: string) -> Result<string, string>

pub fn verify_at(token: string, key: [byte], now_seconds: int, leeway_seconds: int) -> Result<Json, JwtError>

pub fn verify(token: string, key: [byte]) -> Result<Json, JwtError>

pub fn verify(token: string, public_key_pem: string) -> Result<Json, JwtError>

pub fn verify_at(

pub fn verify_claims(

pub fn verify_claims(

pub fn subject(claims: Json) -> Option<string>

pub fn issuer(claims: Json) -> Option<string>

pub fn audience(claims: Json) -> [string]

```

## `locale`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn of(asked: string) -> Locale

pub fn name_of(l: Locale) -> string

pub fn root() -> Locale

pub fn system() -> Locale

pub fn default() -> Locale

pub fn set_default(l: Locale)

pub fn use_system() -> Locale

pub fn month_name(l: Locale, month: int, full: bool) -> string

pub fn day_name(l: Locale, day: int, full: bool) -> string

pub fn decimal_separator(l: Locale) -> string

pub fn thousands_separator(l: Locale) -> string

```

## `log`

```
pub enum Level {
    Debug
    Info
    Warn
    Error
}

pub struct Logger {
    level: Level
    file: Option<string>
    timestamps: bool
    prefix: string
    console: bool
    max_bytes: int
    keep_files: int
    // One JSON object per line instead of the text format, for a log a machine reads. See `write`
    // for the field names and why the escaping goes through `json` rather than `single_line`.
    json: bool
    // Carried on the logger rather than passed to every call, so a request's handler sets it once
    // (`with_request_id`) and every line it logs afterwards carries it -- the same shape as
    // `prefix`, and the reason this is a field and not a parameter. Empty means no id, and the
    // field is then left out of the object rather than written as null, which is friendlier to
    // whatever indexes the log.
    request_id: string
}

pub fn logger(

pub fn with_prefix(l: Logger, prefix: string) -> Logger

pub fn with_request_id(l: Logger, id: string) -> Logger

pub fn rank(level: Level) -> int

pub fn format(level: Level) -> string

pub fn parse_level(text: string) -> Result<Level, string>

pub fn enabled(l: Logger, level: Level) -> bool

pub fn write(l: Logger, level: Level, message: string)

pub fn debug(l: Logger, message: string) { write(l, Level.Debug, message) }

pub fn info(l: Logger, message: string) { write(l, Level.Info, message) }

pub fn warn(l: Logger, message: string) { write(l, Level.Warn, message) }

pub fn error(l: Logger, message: string) { write(l, Level.Error, message) }

```

## `markdown`

**Experimental** (see docs/design/journal/ "Experimental modules"). Markdown as a document tree: a reader over a declared subset, a writer, and the escaping that makes a generated document safe. The writer is the point -- `|` in a table cell, the `*_[]` family in text, a leading `#` or `1.`, a code span fenced longer than whatever is inside it -- so nothing a caller puts in a string can change the structure of the output. Markdown has no syntax errors, so a reader cannot refuse what it does not handle: `parse_markdown` reads anything unsupported as text, and `parse_markdown_strict` returns an Err naming the line instead.

```
pub enum Align {
    AlignNone
    AlignLeft
    AlignRight
    AlignCenter
}

pub struct Link {
    text: [Inline]
    destination: string
    title: Option<string>
}

pub enum Inline {
    Text(string)
    Emphasis([Inline])
    Strong([Inline])
    Code(string)
    Link(Link)
    Image(Link)
    SoftBreak
    HardBreak
}

pub struct Heading {
    level: int
    content: [Inline]
}

pub struct CodeBlock {
    info: string
    text: string
}

pub struct ListItem {
    blocks: [Block]
}

pub struct List {
    ordered: bool
    start: int
    tight: bool
    items: [ListItem]
}

pub struct Table {
    headers: [[Inline]]
    alignments: [Align]
    rows: [[[Inline]]]
}

pub enum Block {
    Heading(Heading)
    Paragraph([Inline])
    CodeBlock(CodeBlock)
    BlockQuote([Block])
    List(List)
    Table(Table)
    ThematicBreak
}

pub struct Document {
    blocks: [Block]
}

pub fn document(blocks: [Block]) -> Document

pub fn heading(level: int, text: string) -> Block

pub fn paragraph(text: string) -> Block

pub fn code_block(info: string, text: string) -> Block

pub fn link(text: string, destination: string) -> Inline

pub fn image(alt: string, destination: string) -> Inline

pub fn text_table(headers: [string], alignments: [Align], rows: [[string]]) -> Result<Block, string>

pub fn text_list(ordered: bool, items: [string]) -> Block

pub fn format(doc: Document) -> Result<string, string>

pub fn to_bytes(doc: Document) -> Result<[byte], string>

pub fn format(block: Block) -> Result<string, string>

pub fn flatten_cell(text: string) -> string

pub fn parse(text: string) -> Result<Document, string>

pub fn parse_strict(text: string) -> Result<Document, string>

pub fn parse(data: [byte]) -> Result<Document, string>

pub fn text_of(doc: Document) -> string

```

## `math`

```
pub val pi = 3.14159265358979323846

pub val e = 2.71828182845904523536

pub native fn abs(x: int) -> int

pub native fn abs(x: float) -> float

pub native fn abs(x: bigint) -> bigint

pub native fn abs(x: decimal) -> decimal

pub native fn sign(x: int) -> int

pub native fn sign(x: float) -> int

pub native fn sign(x: bigint) -> int

pub native fn sign(x: decimal) -> int

pub native fn min<T: Ord>(values: variadic T) -> T

pub native fn max<T: Ord>(values: variadic T) -> T

pub native fn sqrt(x: float) -> float

pub native fn pow(base: float, exponent: float) -> float

pub native fn exp(x: float) -> float

pub native fn log(x: float) -> float

pub native fn log2(x: float) -> float

pub native fn log10(x: float) -> float

pub native fn sin(x: float) -> float

pub native fn cos(x: float) -> float

pub native fn tan(x: float) -> float

pub native fn asin(x: float) -> float

pub native fn acos(x: float) -> float

pub native fn atan(x: float) -> float

pub native fn atan2(y: float, x: float) -> float

pub native fn floor(x: float) -> int

pub native fn ceil(x: float) -> int

pub native fn round(x: float) -> int

pub native fn floor_float(x: float) -> float

pub native fn ceil_float(x: float) -> float

pub native fn round_float(x: float) -> float

pub native fn floor(d: decimal) -> bigint

pub native fn ceil(d: decimal) -> bigint

pub native fn round(d: decimal) -> bigint

pub native fn round(d: decimal, scale: int) -> decimal

pub native fn floor(d: decimal, scale: int) -> decimal

pub native fn ceil(d: decimal, scale: int) -> decimal

pub native fn pow(base: decimal, exponent: int) -> decimal

pub fn is_nan(x: float) -> bool

pub fn is_infinite(x: float) -> bool

pub fn is_finite(x: float) -> bool

```

## `meta`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub enum Type {
    Int
    Float
    Bool
    String
    Bigint
    Decimal
    Byte
    // `unit` is the type with one value; `void` is a function that declares no return type at
    // all. Two variants because they are two things, even though most generators treat them
    // alike.
    Unit
    Void
    Array(Type)
    // **`SetOf`/`MapOf`/`ResultOf`/`ChannelOf` carry the suffix because `Set`, `Map`, `Result`
    // and `Channel` are builtin type names and the language will not take one as a variant
    // name.** The four that collide are the four that are renamed; `Array`, `Func`, `Optional`
    // and `Named` do not, and are left alone rather than renamed for symmetry.
    SetOf(Type)
    ChannelOf(Type)
    MapOf(MapType)
    ResultOf(ResultType)
    Func(FuncType)
    // `Option<T>`. **A variant of its own, which the compiler has no equivalent of**: it
    // represents an `Option` as a struct in some positions and an enum in others, so the old
    // model's `is_option` had to test name and arity and deliberately ignore kind. `meta` is an
    // interface rather than a mirror and may normalise; every generator asks this question
    // first, and now it is a pattern rather than a guess.
    Optional(Type)
    // A user-declared struct or enum. Which of the two it is, is not here: no generator reads
    // the distinction for a type it merely *refers* to.
    Named(NamedType)
    // `T` in `struct Box<T>` -- a type parameter in the body of the declaration that binds it.
    Var(string)
}

pub struct MapType {
    key: Type
    value: Type
}

pub struct ResultType {
    ok: Type
    err: Type
}

pub struct FuncType {
    params: [Type]
    returns: Type
}

pub struct NamedType {
    name: string
    args: [Type]
}

pub fn int_type() -> Type { return Type.Int }

pub fn float_type() -> Type { return Type.Float }

pub fn bool_type() -> Type { return Type.Bool }

pub fn string_type() -> Type { return Type.String }

pub fn bigint_type() -> Type { return Type.Bigint }

pub fn decimal_type() -> Type { return Type.Decimal }

pub fn byte_type() -> Type { return Type.Byte }

pub fn unit_type() -> Type { return Type.Unit }

pub fn void_type() -> Type { return Type.Void }

pub fn array_type(elem: Type) -> Type { return Type.Array(elem) }

pub fn set_type(elem: Type) -> Type { return Type.SetOf(elem) }

pub fn channel_type(elem: Type) -> Type { return Type.ChannelOf(elem) }

pub fn optional_type(elem: Type) -> Type { return Type.Optional(elem) }

pub fn var_type(name: string) -> Type { return Type.Var(name) }

pub fn map_type(key: Type, value: Type) -> Type

pub fn result_type(ok: Type, err: Type) -> Type

pub fn func_type(params: [Type], returns: Type) -> Type

pub fn no_label() -> Option<string> { return Option<string>.None }

pub fn some_label(value: string) -> Option<string> { return Option<string>.Some(value) }

pub fn no_args() -> [AttrArg]

pub fn decl_arg(d: Decl) -> Option<string>

pub fn decl_arg(d: Decl, name: string) -> Option<string>

pub fn decl_attr_arg(d: Decl, name: string) -> Option<AttrArg>

pub fn named_type(name: string, args: [Type]) -> Type

pub struct AttrArg {
    name: string
    value: string
    // How it was written: `"string"`, `"int"` or `"name"`.
    //
    // **Added after the design said it was unnecessary** (docs/design/validation.md §5f), which
    // claimed the value's text was enough because "the compiler hands text over and the generator
    // decides what it means". True of a generator -- and false of anything that has to *show* an
    // attribute, because the lexer takes the quotes off a string, so `name: "start_date"` and
    // `values: type_words` arrive identical. `attr_text` printed one of them wrongly whichever way
    // it guessed, and the generator that exists to show exactly what it was handed
    // (`derive_reflect`) is the wrong place for a cosmetic lie.
    kind: string
}

pub struct Attr {
    name: string
    args: [AttrArg]
}

pub struct Field {
    name: string
    type: Type
    // The attributes written on this field, in source order.
    //
    // **A list of attributes, not a `pk: bool` and a `column: Option<string>`.** `#[pk]` was the
    // only field attribute when this was `[string]`, and that version said an attribute with an
    // argument "is not modelled yet, and will be when one exists rather than guessed at now".
    // One exists: `#[column(name: "start_date")]` (2026-10-29), so the names became
    // `Attr`s with arguments -- the shape the §13 note deferred, settled by a real reader rather
    // than guessed. `has_attr` reads the same as it always did, so a generator asking only
    // whether `#[pk]` is there did not change.
    attrs: [Attr]
}

pub struct Variant {
    name: string
    payload: Option<Type>
}

pub enum Body {
    Struct([Field])
    Enum([Variant])
}

pub struct Decl {
    // The arguments the derive was written with, in source order.
    //
    // **On the declaration rather than a second parameter to `generate`.** Four generators
    // already take one parameter and exactly one target takes an argument; an argument *about*
    // the declaration belongs on it rather than beside it.
    //
    // **A list of named arguments, not one string** (2026-10-30, user-generators.md §5b). It was
    // `arg: Option<string>`, because `Entity` was the only target that took anything and took one
    // bare table name -- which meant `#[derive(Endpoint(method: POST, path: "/orders"))]` could
    // not be written at all. A target now goes through the same reader a field attribute does, so
    // `#[derive(Entity("medications"))]` is one argument named `value` (`#[min(8)]`'s convention)
    // and `decl_arg(d)` is how a generator asks for it.
    args: [AttrArg]
    // The declaration's own name, as written: `LocalDate`.
    name: string
    // The same name in the spelling generated functions use: `local_date`, so a generator writes
    // `local_date_compare` without repeating the compiler's conversion rule.
    snake_name: string
    // The declaration's own type parameters, bare: `["T", "U"]` for `struct Box<T, U>`, empty
    // for one that is not generic. A bound (`<T: Hash>`) is not here -- no generator reads one.
    type_params: [string]
    body: Body
}

pub fn field(name: string, type: Type, attrs: [Attr]) -> Field

pub fn attr(name: string) -> Attr

pub fn attr_with(name: string, args: [AttrArg]) -> Attr

pub struct AttrSpec {
    name: string
    args: string
    // Which declaration may carry it: `"field"` for a struct field, `"function"` for a function.
    // A string rather than an enum to match the rest of this type -- `args` is itself a little
    // grammar in a string, and `AttrArg.kind` is text for the reason its own comment gives.
    on: string
}

pub fn attr_spec(name: string, args: string) -> AttrSpec

pub fn attr_spec(name: string, args: string, on: string) -> AttrSpec

pub fn attr_arg(name: string, value: string, kind: string) -> AttrArg

pub fn has_attr(f: Field, name: string) -> bool

pub fn attr_value(f: Field, attr: string, arg: string) -> Option<string>

pub fn attr_text(a: Attr) -> string

pub fn unit_variant(name: string) -> Variant

pub fn payload_variant(name: string, payload: Type) -> Variant

pub fn struct_decl(

pub fn enum_decl(

pub fn type_text(t: Type) -> string

pub fn bare_name(name: string) -> string

pub fn fresh(n: Mutable<int>, hint: string) -> string

pub fn type_params_bounded(d: Decl, bound: string) -> string

pub fn type_text(d: Decl) -> string

pub struct Param {
    name: string
    type: Type
}

pub struct Function {
    name: string
    params: [Param]
    returns: Type
    // Attributes written on it -- `#[route(method: GET, path: "/api/me/things")]` -- exactly as
    // `Field.attrs` carries a field's. The compiler reads none of them; it only checks that some
    // generator this module imports declared the name for this position (`attr_spec`'s `on`).
    attrs: [Attr]
}

pub struct Target {
    // The entity, reified from wherever it is declared: its fields are the columns, and the ones
    // carrying `#[pk]` are what addresses one row.
    entity: Decl
    // `#![derive(Repository(Medication, label: "Medication"))]`'s label -- the entity as a person says
    // it, for a `NotFound` message that reaches an API response. `None` when none was given, and
    // then the entity's own name is the label.
    label: Option<string>
    // `#![derive(Repository(Notification, crud: false))]` makes this false: name the entity, generate
    // none of the nine CRUD functions. A module with only derived queries wants exactly that.
    crud: bool
    // The functions this module already defines, so a generator can decline to replace one.
    // **A hand-written function wins**, which is what makes the attribute safe to add to a
    // repository that has already been written.
    defined: [string]
}

pub fn param(name: string, type: Type) -> Param

pub fn function(name: string, params: [Param], returns: Type, attrs: [Attr]) -> Function

pub fn fn_has_attr(f: Function, name: string) -> bool

pub fn fn_attr_arg(f: Function, attr: string, arg: string) -> Option<string>

pub struct ModuleTarget {
    // The module's own name, without its folder or `.zm` -- `shared_link` for
    // `controller/shared_link.zm`. **A generator needs it to name what it exports**: a qualified
    // read of another module's val is refused, so thirty modules each exporting a bare `routes`
    // would collide in one import list. Found by writing a generator that had to ask for it as an
    // attribute argument instead.
    name: string
    // `#![derive(Routes(prefix: "/api"))]`'s arguments, as a declaration target's are.
    args: [AttrArg]
    // The module's functions, each with its own `attrs` -- which is where a `#[route(..)]` is read.
    functions: [Function]
    // What the module declares, so a generator skips a name that is already written by hand.
    defined: [string]
}

pub fn module_target(name: string, args: [AttrArg], functions: [Function], defined: [string]) -> ModuleTarget

pub fn module_arg(m: ModuleTarget, name: string) -> Option<string>

pub fn functions_with(m: ModuleTarget, attr: string) -> [Function]

pub fn target(entity: Decl, label: Option<string>, crud: bool, defined: [string]) -> Target

pub fn label_of(t: Target) -> string

pub fn fields_of(t: Target) -> [Field]

pub fn keys_of(t: Target) -> [Field]

pub fn defines(t: Target, name: string) -> bool

```

## `metrics`

```
pub fn counter(name: string, help: string) -> bool

pub fn gauge(name: string, help: string) -> bool

pub fn histogram(name: string, help: string, buckets: [float]) -> bool

pub fn increment(name: string)

pub fn increment(name: string, labels: Map<string, string>)

pub fn increment(s: Series)

pub fn add(name: string, delta: float)

pub fn add(name: string, delta: float, labels: Map<string, string>)

pub fn add(s: Series, delta: float)

pub fn set(name: string, value: float)

pub fn set(name: string, value: float, labels: Map<string, string>)

pub fn set(s: Series, value: float)

pub fn gauge_add(name: string, delta: float)

pub fn gauge_add(name: string, delta: float, labels: Map<string, string>)

pub fn gauge_add(s: Series, delta: float)

pub fn observe(name: string, value: float)

pub fn observe(name: string, value: float, labels: Map<string, string>)

pub fn observe(s: Series, value: float)

pub struct Series {
    handle: int
}

pub fn series(name: string, labels: Map<string, string>) -> Series

pub fn series(name: string) -> Series

pub fn series_of(key_name: string, key: string) -> Series

pub fn render() -> string

pub fn reset()

pub fn record_runtime()

```

## `mime`

Media types (`type/subtype; name=value`): parse, write, match with wildcards, and the type of a file by its extension.

```
pub struct MediaType {
    type: string
    subtype: string
    params: Map<string, string>
}

pub fn application_json() -> MediaType

pub fn application_ndjson() -> MediaType

pub fn application_yaml() -> MediaType

pub fn application_toml() -> MediaType

pub fn application_xml() -> MediaType

pub fn application_form() -> MediaType

pub fn application_octet_stream() -> MediaType

pub fn application_pdf() -> MediaType

pub fn application_zip() -> MediaType

pub fn application_gzip() -> MediaType

pub fn text_plain() -> MediaType

pub fn text_html() -> MediaType

pub fn text_csv() -> MediaType

pub fn image_png() -> MediaType

pub fn image_jpeg() -> MediaType

pub fn image_gif() -> MediaType

pub fn image_svg() -> MediaType

pub fn parse(text: string) -> Result<MediaType, string>

pub fn essence(m: MediaType) -> string

pub fn format(m: MediaType) -> string

pub fn param(m: MediaType, name: string) -> Option<string>

pub fn charset(m: MediaType) -> Option<string>

pub fn with_param(m: MediaType, name: string, value: string) -> MediaType

pub fn suffix(m: MediaType) -> Option<string>

pub fn is_json(m: MediaType) -> bool

pub fn is_text(m: MediaType) -> bool

pub fn matches(pattern: MediaType, candidate: MediaType) -> bool

pub fn of_extension(extension: string) -> Option<MediaType>

pub fn of_path(path: string) -> Option<MediaType>

```

## `ndjson`

Newline-delimited JSON (one value per line) on top of `json`.

```
pub fn parse(text: string) -> Result<[Json], string>

pub fn records(path: string) -> Result<Iter<Result<Json, string>>, string>

pub fn format(items: [Json]) -> string

```

## `net`

```
pub enum NetError {
    ConnectionRefused(string)
    HostNotFound(string)
    Timeout(string)
    Closed(string)
    Io(string)
    Tls(string)
}

pub enum TlsVerify {
    Off
    Chain
    Full
}

pub struct Socket {
    handle: int
}

pub struct Listener {
    handle: int
}

pub fn net_error_message(e: NetError) -> string

pub fn connect(host: string, port: int) -> Result<Socket, NetError>

pub fn connect_timeout(host: string, port: int, timeout_ms: int) -> Result<Socket, NetError>

pub fn connect_unix(path: string) -> Result<Socket, NetError>

pub fn listen_unix(path: string) -> Result<Listener, NetError>

pub fn listen(host: string, port: int) -> Result<Listener, NetError>

pub fn listen_tls(host: string, port: int, cert_file: string, key_file: string) -> Result<Listener, NetError>

pub struct TlsServerOptions {
    cert_file: string
    key_file: string
    client_ca_file: Option<string>
    require_client_cert: bool
    alpn: [string]
}

pub fn tls_server_options(cert_file: string, key_file: string) -> TlsServerOptions

pub fn listen_tls(host: string, port: int, options: TlsServerOptions) -> Result<Listener, NetError>

pub fn listener_port(l: Listener) -> int

pub fn peer_address(s: Socket) -> Option<string>

pub fn wait_readable(sockets: [Socket], timeout_ms: int) -> [int]

pub fn accept(l: Listener) -> Result<Socket, NetError>

pub fn read(s: Socket, max: int) -> Result<[byte], NetError>

pub fn read_exact(s: Socket, count: int) -> Result<[byte], NetError>

pub fn write(s: Socket, data: [byte]) -> Result<int, NetError>

pub fn read_text(s: Socket, max: int) -> Result<string, NetError>

pub fn write_text(s: Socket, text: string) -> Result<int, NetError>

pub fn lines(s: Socket) -> Iter<Result<string, NetError>>

pub fn lines(s: Socket, max_line_bytes: int) -> Iter<Result<string, NetError>>

pub fn set_timeout(s: Socket, millis: int) -> Result<unit, NetError>

pub fn start_tls(s: Socket, host: string) -> Result<unit, NetError>

pub fn start_tls(s: Socket, host: string, verify: TlsVerify, ca_file: Option<string>) -> Result<unit, NetError>

pub struct TlsClientOptions {
    verify: TlsVerify
    ca_file: Option<string>
    cert_file: Option<string>
    key_file: Option<string>
    alpn: [string]
}

pub fn tls_client_options() -> TlsClientOptions

pub fn start_tls(s: Socket, host: string, options: TlsClientOptions) -> Result<unit, NetError>

pub fn connect_tls(host: string, port: int) -> Result<Socket, NetError>

pub fn connect_tls(

pub fn connect_tls(host: string, port: int, options: TlsClientOptions) -> Result<Socket, NetError>

pub fn negotiated_protocol(s: Socket) -> Option<string>

pub fn peer_certificate_subject(s: Socket) -> Option<string>

pub fn close(s: Socket)

pub fn close(l: Listener)

```

## `number`

Shared formatting/parsing across `int`/`float`/`bigint`/`decimal`/`byte`, written in pure Ziman on top of each type's own `str()`. `NumberFormat`/`parse_pattern` cover thousands separators and forced decimal places (a small subset of DecimalFormat-style patterns); `byte_to_hex`/`int_to_hex`/`bigint_to_hex` (and their `_from_hex` inverses) cover hex for the three integral types.

```
pub val byte_min = 0b

pub val byte_max = 255b

pub val int_min = -9223372036854775808

pub val int_max = 9223372036854775807

pub val float_min = -1.7976931348623157e+308

pub val float_max = 1.7976931348623157e+308

pub val float_min_positive = 5e-324

pub native fn float_to_text(x: float) -> string

pub native fn to_hex(b: byte) -> string

pub native fn byte_from_hex(s: string) -> Result<byte, string>

pub native fn to_hex(x: int) -> string

pub native fn int_from_hex(s: string) -> Result<int, string>

pub native fn to_hex(data: [byte]) -> string

pub native fn bytes_from_hex(s: string) -> Result<[byte], string>

pub fn to_hex(n: bigint) -> string

pub fn bigint_from_hex(s: string) -> Result<bigint, string>

pub fn parse_byte(s: string) -> Result<byte, string>

pub struct NumberFormat {
    thousands_separator: string
    decimal_separator: string
    // **The fraction's shape, as `0` and `#` after the point state it.** `min_decimal_places` is
    // how many places are always written (the `0`s); `max_decimal_places` is the most allowed (the
    // `0`s plus the `#`s), and `None` means the value keeps its own. `.00` is min 2 / max 2 -- a
    // fixed two places; `.##` is min 0 / max 2 -- up to two, trailing zeros dropped; `.0#` is
    // min 1 / max 2. A pattern with no point at all is min 0 / max None.
    //
    // Until 2026-10-24 this was one `decimal_places: Option<int>`, which could only say "exactly
    // N", so `#,##0.00` and `#,###.##` formatted identically -- the one piece of real
    // `DecimalFormat` semantics that was missing rather than merely absent.
    min_decimal_places: int
    max_decimal_places: Option<int>
    // The integer side's minimum width, zero-padded: the count of `0`s in the integer part, so
    // `00000` writes 42 as `00042`. It only ever pads -- a value whose integer part is `0` keeps
    // that digit whatever this says, since a number has to have one.
    min_integer_digits: int
}

pub fn number_format_for(l: Locale) -> NumberFormat

pub fn number_format_plain() -> NumberFormat

pub fn parse_pattern(pattern: string) -> Result<NumberFormat, string>

pub fn parse_pattern(pattern: string, l: Locale) -> Result<NumberFormat, string>

pub fn format(s: string, fmt: NumberFormat) -> string

pub fn parse(s: string, fmt: NumberFormat) -> string

pub fn format(x: int, fmt: NumberFormat) -> string

pub fn format(x: float, fmt: NumberFormat) -> string

pub fn format(x: bigint, fmt: NumberFormat) -> string

pub fn format(x: decimal, fmt: NumberFormat) -> string

pub fn format(x: byte, fmt: NumberFormat) -> string

pub native fn u16_to_bytes(n: int) -> [byte]

pub native fn u32_to_bytes(n: int) -> [byte]

pub native fn u16_from_bytes(b: [byte], offset: int) -> int

pub native fn u32_from_bytes(b: [byte], offset: int) -> int

pub native fn i32_from_bytes(b: [byte], offset: int) -> int

pub native fn i64_from_bytes(b: [byte], offset: int) -> int

pub native fn f64_from_bytes(b: [byte], offset: int) -> float

pub native fn f32_from_bytes(b: [byte], offset: int) -> float

pub native fn f64_to_bytes(f: float) -> [byte]

```

## `os`

```
pub fn get_env(name: string) -> Option<string>

pub native fn set_env(name: string, value: string) -> bool

pub native fn args() -> [string]

pub fn cwd() -> Result<string, string>

pub native fn pid() -> int

pub native fn collect_garbage()

pub native fn collector_enabled() -> bool

pub native fn open_descriptors() -> int

pub native fn exit(code: int)

pub fn fatal(message: string)

pub native fn write_stderr(text: string)

pub native fn install_shutdown_handler()

pub native fn shutdown_requested() -> bool

pub native fn cpu_count() -> int

pub native fn hostname() -> string

pub native fn name() -> string

pub native fn arch() -> string

pub native fn load_average() -> Result<[float], string>

pub native fn uptime_seconds() -> Result<int, string>

pub native fn memory_total() -> Result<int, string>

pub native fn memory_available() -> Result<int, string>

pub native fn process_memory() -> Result<int, string>

pub native fn disk_total(path: string) -> Result<int, string>

pub native fn disk_free(path: string) -> Result<int, string>

pub native fn disk_used(path: string) -> Result<int, string>

pub struct CpuTimes {
    busy: int
    total: int
}

pub fn cpu_times() -> Result<CpuTimes, string>

pub fn cpu_usage(before: CpuTimes, after: CpuTimes) -> float

pub fn cpu_usage(sample_millis: int) -> Result<float, string>

pub native fn process_cpu_seconds() -> Result<float, string>

pub native fn process_uptime_seconds() -> Result<int, string>

pub native fn thread_count() -> int

```

## `otp`

Time-based one-time passwords (RFC 6238): the six-digit codes of an authenticator app -- generate a secret, a provisioning URI, and check a code.

```
pub fn generate_secret() -> string

pub fn base32_encode(data: [byte]) -> string

pub fn base32_decode(text: string) -> Result<[byte], string>

pub fn hotp(key: [byte], counter: int, digits: int) -> string

pub fn totp(key: [byte], unix_seconds: int) -> string

pub fn totp(key: [byte], unix_seconds: int, digits: int) -> string

pub fn totp_matching_step(key: [byte], code: string, unix_seconds: int, window: int) -> Option<int>

pub fn totp_matching_step(

pub fn totp_valid(key: [byte], code: string, unix_seconds: int, window: int) -> bool

pub fn totp_valid(key: [byte], code: string, unix_seconds: int, window: int, digits: int) -> bool

pub fn provisioning_uri(issuer: string, account: string, secret: string) -> string

```

## `process`

```
pub struct Process {
    handle: int
}

pub struct EnvVar {
    name: string
    value: string
}

pub fn env(name: string, value: string) -> EnvVar

pub fn start(

pub fn wait(p: Process) -> Result<int, string>

pub fn terminate(p: Process) -> Result<unit, string>

pub fn kill(p: Process) -> Result<unit, string>

pub fn interrupt(p: Process) -> Result<unit, string>

pub fn wait_for(p: Process, millis: int) -> Result<Option<int>, string>

pub fn release(p: Process)

pub fn pid(p: Process) -> int

pub fn run(

pub fn output(

```

## `properties`

```
pub struct Entry {
    key: string
    value: string
    source: string
}

pub struct Properties {
    entries: [Entry]
    profile: string
}

pub fn lookup(props: Properties, key: string) -> Option<Entry>

pub fn parse(text: string, source: string) -> Result<Properties, string>

pub fn load(name: string) -> Result<Properties, string>

pub fn get_string(e: Entry) -> Result<string, string>

pub fn get_string(props: Properties, key: string) -> Result<string, string>

pub fn get_int(e: Entry) -> Result<int, string>

pub fn get_int(props: Properties, key: string) -> Result<int, string>

pub fn get_float(e: Entry) -> Result<float, string>

pub fn get_float(props: Properties, key: string) -> Result<float, string>

pub fn get_bool(e: Entry) -> Result<bool, string>

pub fn get_bool(props: Properties, key: string) -> Result<bool, string>

pub fn key(prefix: string, name: string) -> string

pub fn check_unknown(props: Properties, known: variadic string) -> Result<bool, string>

pub fn report(props: Properties, keys: variadic string) -> string

```

## `random`

One native function reading real OS entropy (`/dev/urandom`) -- Ziman's first source of genuine non-determinism.

```
pub native fn bytes(n: int) -> [byte]

pub fn below(n: int) -> int

pub fn between(low: int, high: int) -> int

pub fn fraction() -> float

pub fn boolean() -> bool

pub fn choice<T>(items: [T]) -> Option<T>

pub fn shuffle<T>(items: [T]) -> [T]

pub fn text(n: int, alphabet: string) -> string

pub fn alpha(n: int) -> string

pub fn numeric(n: int) -> string

pub fn alpha_numeric(n: int) -> string

```

## `regex`

```
pub struct Regex {
    handle: int
    groups: int
    pattern: string
    names: [string]
}

pub struct Match {
    start: int
    end: int
    text: string
    groups: [Option<string>]
    names: [string]
}

pub fn compile(pattern: string) -> Result<Regex, string>

pub fn is_match(re: Regex, text: string) -> bool

pub fn find_from(re: Regex, text: string, from: int) -> Option<Match>

pub fn find(re: Regex, text: string) -> Option<Match>

pub fn find_all(re: Regex, text: string) -> [Match]

pub fn group(m: Match, n: int) -> Option<string>

pub fn group_named(m: Match, name: string) -> Option<string>

pub fn replace(re: Regex, text: string, replacement: string) -> string

pub fn replace_all(re: Regex, text: string, replacement: string) -> string

pub fn split(re: Regex, text: string) -> [string]

pub fn escape(text: string) -> string

```

## `template`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub struct Sheet {
    templates: [Named]
}

pub struct Row {
    cells: [Binding]
}

pub struct Binding {
    name: string
    value: Json
}

pub fn text(name: string, value: string) -> Binding

pub fn flag(name: string, value: bool) -> Binding

pub fn rows(name: string, value: [Row]) -> Binding

pub fn row(cells: [Binding]) -> Row

pub fn context(bindings: [Binding]) -> Json

pub fn parse(source: string) -> Result<Sheet, string>

pub fn must_parse(source: string) -> Sheet

pub fn render(sheet: Sheet, name: string, ctx: Json) -> Result<string, string>

pub fn render_text(source: string, ctx: Json) -> Result<string, string>

pub fn render(sheet: Sheet, name: string, bindings: [Binding]) -> Result<string, string>

```

## `test`

```
pub fn fail(message: string)

pub fn assert(cond: bool)

pub fn assert_false(cond: bool)

pub fn assert_eq<T: Eq>(expected: T, actual: T)

pub fn assert_ne<T: Eq>(unexpected: T, actual: T)

pub fn assert_near(expected: float, actual: float, tolerance: float)

pub fn assert_lt<T: Ord>(actual: T, bound: T)

pub fn assert_le<T: Ord>(actual: T, bound: T)

pub fn assert_gt<T: Ord>(actual: T, bound: T)

pub fn assert_ge<T: Ord>(actual: T, bound: T)

pub fn assert_contains(haystack: string, needle: string)

pub fn assert_contains<T: Eq>(items: [T], x: T)

pub fn assert_contains<T: Eq + Hash>(items: Set<T>, x: T)

pub fn assert_contains<K: Eq + Hash, V>(m: Map<K, V>, key: K)

pub fn assert_not_contains(haystack: string, needle: string)

pub fn assert_not_contains<T: Eq>(items: [T], x: T)

pub fn assert_not_contains<T: Eq + Hash>(items: Set<T>, x: T)

pub fn assert_not_contains<K: Eq + Hash, V>(m: Map<K, V>, key: K)

pub fn assert_len(expected: int, items: string)

pub fn assert_len<T>(expected: int, items: [T])

pub fn assert_len<T: Eq + Hash>(expected: int, items: Set<T>)

pub fn assert_len<K: Eq + Hash, V>(expected: int, m: Map<K, V>)

pub fn assert_empty(items: string)

pub fn assert_empty<T>(items: [T])

pub fn assert_empty<T: Eq + Hash>(items: Set<T>)

pub fn assert_empty<K: Eq + Hash, V>(m: Map<K, V>)

pub fn assert_not_empty(items: string)

pub fn assert_not_empty<T>(items: [T])

pub fn assert_not_empty<T: Eq + Hash>(items: Set<T>)

pub fn assert_not_empty<K: Eq + Hash, V>(m: Map<K, V>)

pub fn assert_all_match<T>(items: [T], test: fn(T) -> bool)

pub fn assert_any_match<T>(items: [T], test: fn(T) -> bool)

pub fn assert_none_match<T>(items: [T], test: fn(T) -> bool)

pub fn assert_entry<K: Eq + Hash, V: Eq>(m: Map<K, V>, key: K, value: V)

pub fn assert_starts_with(s: string, prefix: string)

pub fn assert_ends_with(s: string, suffix: string)

pub fn assert_blank(s: string)

pub fn assert_not_blank(s: string)

pub fn assert_matches(s: string, pattern: string)

pub fn assert_not_matches(s: string, pattern: string)

pub fn assert_some<T>(opt: Option<T>) -> T

pub fn assert_none<T>(opt: Option<T>)

pub fn assert_ok<T, E>(r: Result<T, E>) -> T

pub fn assert_err<T, E>(r: Result<T, E>) -> E

```

## `thread`

```
pub native fn id() -> int

pub native fn name() -> string

pub native fn set_name(name: string)

pub native fn is_main() -> bool

pub native fn stack_remaining() -> int

pub struct ThreadLocal<T> {
    id: int
    init: fn() -> T
}

pub fn local<T>(init: fn() -> T) -> ThreadLocal<T>

pub fn get<T>(key: ThreadLocal<T>) -> T

pub fn set<T>(key: ThreadLocal<T>, value: T)

pub fn has<T>(key: ThreadLocal<T>) -> bool

pub fn reset<T>(key: ThreadLocal<T>)

```

## `toml`

**Experimental** (see docs/design/journal/ "Experimental modules"). TOML 1.0 to and from a `Json` tree. Dates stay strings; `inf`/`nan` are rejected.

```
pub fn parse(text: string) -> Result<Json, string>

pub fn format(j: Json) -> Result<string, string>

```

## `url`

```
pub fn percent_decode(s: string, plus_is_space: bool) -> Result<string, string>

pub fn percent_encode(s: string, plus_for_space: bool) -> string

```

## `uuid`

RFC 4122 v4 UUIDs, written in pure Ziman on top of `random` -- version/variant bits set with plain integer arithmetic, since Ziman has no bitwise operators.

```
pub struct Uuid {
    bytes: [byte]
}

pub fn uuid_new() -> Uuid

pub fn uuid_new_v7() -> Uuid

pub fn timestamp_millis(u: Uuid) -> Option<int>

pub fn uuid_of(bytes: [byte]) -> Result<Uuid, string>

pub fn uuid_nil() -> Uuid

pub fn uuid_max() -> Uuid

pub fn is_nil(u: Uuid) -> bool

pub fn format(u: Uuid) -> string

pub fn parse(s: string) -> Result<Uuid, string>

pub fn version(u: Uuid) -> int

pub fn uuid_to_json(u: Uuid) -> Json

pub fn uuid_from_json(j: Json) -> Result<Uuid, string>

pub fn uuid_to_sql(u: Uuid) -> SqlValue

pub fn uuid_from_sql(v: SqlValue) -> Result<Uuid, DbError>

pub fn uuid_compare(a: Uuid, b: Uuid) -> int

```

## `validate`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub struct Violation {
    field: string
    message: string
}

pub fn violation(field: string, message: string) -> Violation

pub fn first(messages: [Option<string>]) -> Option<string>

pub fn with_message(found: Option<string>, message: string) -> Option<string>

pub fn not_blank(value: string) -> Option<string>

pub fn not_blank(value: Option<string>) -> Option<string>

pub fn not_empty(value: string) -> Option<string>

pub fn not_empty<T>(values: [T]) -> Option<string>

pub fn not_empty<T>(values: Option<[T]>) -> Option<string>

pub fn one_of(value: string, allowed: [string]) -> Option<string>

pub fn one_of(value: Option<string>, allowed: [string]) -> Option<string>

pub fn one_of(values: [string], allowed: [string]) -> Option<string>

pub fn one_of(values: Option<[string]>, allowed: [string]) -> Option<string>

pub fn size_at_least(value: string, min: int) -> Option<string>

pub fn size_at_least(value: Option<string>, min: int) -> Option<string>

pub fn size_at_most(value: string, max: int) -> Option<string>

pub fn size_at_most(value: Option<string>, max: int) -> Option<string>

pub fn size_between(value: string, min: int, max: int) -> Option<string>

pub fn size_between(value: Option<string>, min: int, max: int) -> Option<string>

pub fn at_least(value: int, bound: int) -> Option<string>

pub fn at_least(value: Option<int>, bound: int) -> Option<string>

pub fn at_most(value: int, bound: int) -> Option<string>

pub fn at_most(value: Option<int>, bound: int) -> Option<string>

pub fn positive(value: int) -> Option<string>

pub fn positive(value: Option<int>) -> Option<string>

pub fn positive(value: float) -> Option<string>

pub fn positive(value: Option<float>) -> Option<string>

pub fn email(value: string) -> Option<string>

pub fn email(value: Option<string>) -> Option<string>

pub fn is_email(value: string) -> bool

pub fn matches(value: string, pattern: string) -> Option<string>

pub fn matches(value: Option<string>, pattern: string) -> Option<string>

```

## `xml`

**Experimental** (see docs/design/journal/ "Experimental modules"). XML 1.0 as a document tree: a reader, a writer and a small path finder, in pure Ziman. Not a `Json` tree -- XML interleaves text with elements, separates attributes from children and identifies a name by its namespace URI, none of which survive a map. There is no DTD at all, so `<!DOCTYPE>` is a parse error and entity-expansion and external-entity attacks have nowhere to stand; size, depth and attribute-count limits are in `XmlLimits`.

```
pub struct Name {
    namespace: Option<string>
    local: string
    prefix: Option<string>
}

pub struct Attribute {
    name: Name
    value: string
}

pub struct Instruction {
    target: string
    data: string
}

pub enum Node {
    Element(Element)
    Text(string)
    CData(string)
    Comment(string)
    ProcessingInstruction(Instruction)
}

pub struct Element {
    name: Name
    attributes: [Attribute]
    children: [Node]
}

pub struct Declaration {
    version: string
    encoding: Option<string>
    standalone: Option<bool>
}

pub struct Document {
    declaration: Option<Declaration>
    prolog: [Node]
    root: Element
    epilog: [Node]
}

pub struct XmlLimits {
    max_bytes: int
    max_depth: int
    max_attributes: int
}

pub fn xml_limits() -> XmlLimits

pub fn name(local: string) -> Name

pub fn ns_name(uri: string, prefix: string, local: string) -> Name

pub fn element(n: Name, attributes: [Attribute], children: [Node]) -> Element

pub fn empty(local: string) -> Element

pub fn text_element(local: string, content: string) -> Element

pub fn opt_element(local: string, value: Option<string>) -> Option<Node>

pub fn req_element(local: string, value: string) -> Option<Node>

pub fn attribute(local: string, value: string) -> Attribute

pub fn document(root: Element) -> Document

pub fn same_name(a: Name, b: Name) -> bool

pub fn attr(e: Element, local: string) -> Option<string>

pub fn attr_ns(e: Element, uri: string, local: string) -> Option<string>

pub fn attr_or(e: Element, local: string, fallback: string) -> string

pub fn children(e: Element) -> [Element]

pub fn children_named(e: Element, local: string) -> [Element]

pub fn child(e: Element, local: string) -> Option<Element>

pub fn text_of(e: Element) -> string

pub fn find(e: Element, path: string) -> [Element]

pub fn find_ns(e: Element, uri: string, path: string) -> [Element]

pub fn find_first(e: Element, path: string) -> Option<Element>

pub fn parse(text: string) -> Result<Document, string>

pub fn parse(text: string, limits: XmlLimits) -> Result<Document, string>

pub fn parse(data: [byte]) -> Result<Document, string>

pub fn parse(data: [byte], limits: XmlLimits) -> Result<Document, string>

pub fn format(doc: Document) -> Result<string, string>

pub fn format(doc: Document, indent: string) -> Result<string, string>

pub fn xml_to_bytes(doc: Document) -> Result<[byte], string>

pub fn format(e: Element) -> Result<string, string>

```

## `yaml`

**Experimental** (see docs/design/journal/ "Experimental modules"). A strict YAML 1.2 subset to and from a `Json` tree: block and flow collections, quoted/plain/block scalars. Anchors, tags, merge keys and multiple documents are errors.

```
pub fn parse(text: string) -> Result<Json, string>

pub fn format(j: Json) -> string

```

## `datasource`

```
pub struct Registry {
    drivers: [Driver]
}

pub fn registry(drivers: [Driver]) -> Registry

pub enum Connection {
    Direct(Conn)
    Pool(PoolState)
}

pub fn dialect(conn: Connection) -> string

pub fn open(reg: Registry, url: string) -> Result<Connection, DbError>

pub fn close(conn: Connection)

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

pub fn query_one(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Option<Row>, DbError>

pub fn begin(conn: Connection) -> Result<unit, DbError>

pub fn commit(conn: Connection) -> Result<unit, DbError>

pub fn rollback(conn: Connection) -> Result<unit, DbError>

pub fn open_pool(reg: Registry, url: string, size: int) -> Result<Connection, DbError>

pub fn open_pool_with(reg: Registry, url: string, size: int, check_after_seconds: int) -> Result<Connection, DbError>

pub fn acquire(conn: Connection) -> Result<Connection, DbError>

pub fn acquire_timeout(conn: Connection, timeout_ms: int) -> Result<Connection, DbError>

pub fn release(conn: Connection, taken: Connection)

pub fn discard(conn: Connection, taken: Connection)

pub fn with_transaction<R>(conn: Connection, work: fn(Connection) -> Result<R, DbError>) -> Result<R, DbError>

```

## `db`

```
pub struct Conn {
    dialect: string
    close: fn()
    execute: fn(string, [SqlValue]) -> Result<int, DbError>
    query: fn(string, [SqlValue]) -> Result<[Row], DbError>
    rows: fn(string, [SqlValue]) -> Result<Iter<Result<Row, DbError>>, DbError>
}

pub struct Driver {
    name: string
    prefixes: [string]
    poolable: fn(string) -> bool
    idle_check: bool
    open: fn(string) -> Result<Conn, DbError>
}

pub enum SqlValue {
    Null
    Int(int)
    Float(float)
    Text(string)
    Bool(bool)
    Bytes([byte])
}

pub enum DbError {
    NotFound(string)
    Constraint(string)
    Connection(string)
    Sql(string)
}

pub struct Row {
    columns: [string]
    values: [SqlValue]
}

pub fn db_error_message(e: DbError) -> string

pub fn row_get(row: Row, column: string) -> Result<SqlValue, DbError>

pub fn sql_as_text(v: SqlValue) -> Result<string, DbError>

pub fn sql_as_int(v: SqlValue) -> Result<int, DbError>

pub fn sql_as_bool(v: SqlValue) -> Result<bool, DbError>

pub fn sql_as_float(v: SqlValue) -> Result<float, DbError>

pub fn sql_as_decimal(v: SqlValue) -> Result<decimal, DbError>

pub fn sql_lift<T>(r: Result<T, string>, what: string) -> Result<T, DbError>

pub struct UrlParam {
    name: string
    value: string
}

pub struct DbUrl {
    user: string
    password: string
    host: string
    port: int
    database: string
    params: [UrlParam]
}

pub fn percent_decode(s: string) -> Result<string, DbError>

pub fn url_param(u: DbUrl, name: string) -> Option<string>

pub fn parse_url(url: string, prefixes: [string], default_port: int) -> Result<DbUrl, DbError>

pub struct QueryResult {
    rows: [Row]
    affected: int
}

pub fn open_rows(

```

## `derive_entity`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn attributes() -> [meta.AttrSpec]

pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_repository`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn generate_module(t: meta.Target) -> Result<string, string>

pub fn generate_query(t: meta.Target, f: meta.Function) -> Result<string, string>

```

## `derive_row`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn emit_to_sql(t: meta.Type, src: string, dst: string, n: Mutable<int>) -> Result<string, string>

pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `eql`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub struct Entity {
    name: string
    table: string
    columns: [string]
    // The columns that identify one row, in field order: the fields marked `#[pk]`.
    //
    // **Empty is allowed and means "no key was declared"**, not "the first column". A reader that
    // needs one -- anything addressing a single row -- refuses an empty `keys` by name rather than
    // guessing, because the guess was right for all 27 entities in the North Star application and
    // would have been silently wrong for the first table that is keyed on something else.
    //
    // More than one is a compound key, which is simply two fields marked. Nothing here objects to
    // that; a *reader* that can only address a single-column key is the thing that has to say so.
    keys: [string]
    // The struct field each column belongs to, in the same order -- what a *query* names where
    // `columns` is what the *database* has.
    //
    // **Equal to `columns` unless a field carries `#[column(name: "..")]`** (2026-10-29), and the
    // paragraph above this struct is why that matters: field name, column name, what a query may
    // write and what `from_row` reads were one list "by construction", and an override makes them
    // two. So a query still says `e.start`, this pairs it with `start_date`, and the column name
    // appears in the SQL and nowhere a reader of the struct has to think about it.
    //
    // The two lists are parallel. Nothing here enforces that, because the only thing that builds a
    // mismatched pair is a caller writing one by hand, and `column_for` falls back to the field's
    // own name rather than reading past the end of `columns`.
    fields: [string]
}

pub fn entity(name: string, table: string, columns: [string]) -> Entity

pub fn entity(name: string, table: string, columns: [string], fields: [string]) -> Entity

pub fn keyed_entity(name: string, table: string, columns: [string], keys: [string]) -> Entity

pub fn keyed_entity(

pub struct Registry {
    // **Keyed on the folded name**, because SQL identifiers are case-insensitive and every lookup
    // folds what it was given. Folding happens once per entity here instead of once per entity per
    // comparison, which is what a linear scan over this was doing.
    by_name: Map<string, Entity>
    // The names as they were registered, in order, for the refusal message. The keys above are
    // folded and a map has no order, so "registered: vitalsign, bodymeasurement" is neither what
    // the caller wrote nor the order they wrote it in.
    names: [string]
}

pub fn registry(entities: [Entity]) -> Registry

pub struct Binding {
    name: string
    value: SqlValue
    values: [SqlValue]
    list: bool
}

pub fn bind(name: string, value: SqlValue) -> Binding

pub fn bind(name: string, value: int) -> Binding

pub fn bind(name: string, value: float) -> Binding

pub fn bind(name: string, value: string) -> Binding

pub fn bind(name: string, value: bool) -> Binding

pub fn bind(name: string, value: [byte]) -> Binding

pub fn bind(name: string, value: Uuid) -> Binding

pub fn bind(name: string, value: Instant) -> Binding

pub fn bind(name: string, value: LocalDate) -> Binding

pub fn bind(name: string, value: LocalTime) -> Binding

pub fn bind(name: string, value: LocalDateTime) -> Binding

pub fn bind(name: string, value: Duration) -> Binding

pub fn bind(name: string, value: ZoneOffset) -> Binding

pub fn bind(name: string, value: ZoneId) -> Binding

pub fn bind_list(name: string, values: [SqlValue]) -> Binding

pub fn bind_list(name: string, values: [string]) -> Binding

pub fn bind_list(name: string, values: [int]) -> Binding

pub fn bind_list(name: string, values: [Uuid]) -> Binding

pub struct Translated {
    sql: string
    statement: Statement
    bindings: [string]
    lists: [string]
    query: string
    registry: Registry
    // The sizes already worked out, for a query with a collection parameter. See `Shaped`.
    shaped: [Shaped]
}

pub fn translate(reg: Registry, query: string) -> Result<Translated, DbError>

pub fn arrange(t: Translated, bound: [Binding]) -> Result<[SqlValue], DbError>

pub fn prepare(query: Translated, bound: [Binding]) -> Result<Prepared, DbError>

pub struct Prepared {
    sql: string
    params: [SqlValue]
}

pub fn list<E>(

pub fn one<E>(

pub fn insert_one<E>(

pub fn update_one<E>(

pub fn find_by_key<E>(

pub fn find_maybe<E>(

pub fn exists(conn: datasource.Connection, e: Entity, keys: [SqlValue]) -> Result<bool, DbError>

pub fn find_all<E>(

pub fn count_all(conn: datasource.Connection, e: Entity) -> Result<int, DbError>

pub fn delete_where(conn: datasource.Connection, e: Entity, matching: [Binding]) -> Result<int, DbError>

pub fn delete_one(

pub fn delete_all(conn: datasource.Connection, e: Entity) -> Result<int, DbError>

pub fn execute(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<int, DbError>

pub fn rows(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<[Row], DbError>

pub fn count(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<int, DbError>

```

## `memdb`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn driver() -> Driver

pub fn strict_driver() -> Driver

pub struct Template {
    database: Database
    strict: bool
}

pub fn template(url: string, setup: [string]) -> Result<Template, DbError>

pub fn seeded(t: Template) -> Driver

```

## `migrate`

Versioned schema migrations: a directory of V<n>__<name>.sql files applied in order, once each, in one transaction, with checksums to catch an edited migration.

```
pub struct Migration {
    version: int
    name: string
    file: string
    sql: string
    checksum: string
}

pub fn checksum(sql: string) -> string

pub fn migration(version: int, name: string, sql: string) -> Migration

pub struct TypeToken {
    token: string
    // What each engine spells this token as, keyed by the dialect `datasource.dialect` answers.
    //
    // **A map rather than a field per engine.** It was `sqlite`/`postgres`/`mysql` as three
    // fields with a three-branch lookup, so a fourth engine meant editing this struct, the table
    // and the lookup -- which is exactly the shape `datasource` lost when a driver became a value.
    // The first thing `memdb` did on meeting a real migration directory was fail here, with
    // "uses {{uuid}} and there is no spelling of it for 'memdb'", which is the right error and
    // was being raised by the wrong design.
    spellings: Map<string, string>
}

pub val type_tokens = [

pub fn substitute(sql: string, dialect: string, file: string) -> Result<string, DbError>

pub fn split_statements(sql: string) -> Result<[string], DbError>

pub fn load(dir: string) -> Result<[Migration], DbError>

pub fn load_for(dir: string, dialect: string) -> Result<[Migration], DbError>

pub fn apply(conn: Connection, migrations: [Migration]) -> Result<int, DbError>

pub fn run(conn: Connection, dir: string) -> Result<int, DbError>

```

## `mysql`

**Experimental** (see docs/design/journal/ "Experimental modules"). A MySQL/MariaDB client, the third engine behind `datasource`: the text and binary protocols, `mysql_native_password`/`caching_sha2_password` authentication, TLS. No compressed protocol, LOCAL INFILE, the RSA key exchange, or prepared-statement reuse.

```
pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}

pub fn connect(url: string) -> Result<Connection, DbError>

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn close(conn: Connection)

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

pub fn driver() -> Driver

```

## `pg`

**Experimental** (see docs/design/journal/ "Experimental modules"). A PostgreSQL client, the second engine behind `datasource`: the simple and extended query protocols, trust/cleartext/SCRAM-SHA-256 authentication, TLS. No MD5 auth, Kerberos, client certificates, COPY or LISTEN/NOTIFY.

```
pub native fn float_to_text(f: float) -> string

pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}

pub fn connect(url: string) -> Result<Connection, DbError>

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn close(conn: Connection)

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

pub fn driver() -> Driver

```

## `sqlite`

```
pub struct Connection {
    handle: int
}

pub fn open(path: string) -> Result<Connection, DbError>

pub fn close(conn: Connection)

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

pub fn driver() -> Driver

```

## `sql`

**Experimental** (see docs/design/journal/ "Experimental modules"). 

```
pub fn fold_name(name: string) -> string

pub struct Token {
    kind: string
    text: string
    // `text` upper-cased once, for a `name` token, and empty for every other kind.
    //
    // **The parser asks "is this word FROM?" far more often than there are tokens** -- a single
    // bare alias runs the whole `clause_words` list past one token -- and each such question used
    // to upper-case the token's text again, allocating a string to throw away. Folding once, here,
    // is what turns that from a per-question cost into a per-token one.
    upper: string
    value: int
    // Where this token sits in the source, so a run of tokens can be recovered as the text it was
    // written as. That is what lets the parts this module does not model be **preserved verbatim**
    // rather than re-assembled from tokens with guessed spacing.
    start: int
    end: int
}

pub fn tokenize(sql: string) -> Result<[Token], DbError>

pub enum ConstraintRule {
    PrimaryKey([string])
    Unique([string])
    Check(Expr)
    ForeignKey(ForeignKeyRule)
    // A shape this module does not model, carried by the constraint's `text` and written back
    // verbatim.
    Opaque
}

pub struct ForeignKeyRule {
    // The child's columns -- this table's.
    columns: [string]
    table: string
    // The parent's columns. Empty where none were named, which means the parent's primary key.
    references: [string]
    // `CASCADE`, `SET NULL`, `SET DEFAULT`, `RESTRICT`, `NO ACTION`, or empty for the default --
    // which *is* `NO ACTION`, spelled differently so a reader can tell "not written" from
    // "written as the default".
    on_delete: string
    on_update: string
}

pub struct TableConstraint {
    // From `CONSTRAINT <name>`, or empty. An engine quotes it in its message when it is there.
    name: string
    rule: ConstraintRule
    // The constraint exactly as written, brackets and spacing included. `format` writes it for an
    // `Opaque` rule, and an engine's message quotes the expression inside it for an unnamed
    // `CHECK` -- which is what SQLite quotes, spacing and all.
    text: string
}

pub struct Column {
    name: string
    type_name: string
    not_null: bool
    primary_key: bool
    unique: bool
    default_value: Option<SqlValue>
    // The constraints written in this column's own position that this module models: a bare
    // `CHECK (e)` and a bare `REFERENCES t (c)`, each with this column as its column list. They
    // stay here rather than moving to the table's list so `format` writes them back where they
    // were -- the sample application's migrations put every foreign key in column position.
    //
    // A *named* column constraint (`CONSTRAINT k CHECK (..)`) is **not** modelled and goes to
    // `extra` whole, so nothing of it is lost and nothing of it is enforced.
    constraints: [TableConstraint]
    // Every constraint on this column that is modelled by neither the four flags above nor
    // `constraints`, as the text it was written as: `COLLATE NOCASE`, `AUTOINCREMENT`,
    // `GENERATED ALWAYS AS ..`, a named constraint. Preserved rather than modelled, so `format`
    // writes back what it read.
    extra: string
}

pub struct BinNode {
    op: string
    left: Expr
    right: Expr
}

pub struct NotNode {
    inner: Expr
}

pub struct NullNode {
    inner: Expr
    negated: bool
}

pub struct ColRef {
    table: string
    name: string
}

pub struct CallNode {
    name: string
    args: [Expr]
    // `COUNT(DISTINCT x)`: the duplicates are dropped before the aggregate sees them.
    distinct: bool
}

pub struct LikeNode {
    subject: Expr
    pattern: Expr
    negated: bool
}

pub struct InNode {
    subject: Expr
    options: [Expr]
    negated: bool
}

pub struct BetweenNode {
    subject: Expr
    low: Expr
    high: Expr
    negated: bool
}

pub struct WhenArm {
    condition: Expr
    value: Expr
}

pub struct CaseNode {
    arms: [WhenArm]
    fallback: [Expr]
}

pub enum Expr {
    Lit(SqlValue)
    Col(ColRef)
    Param(int)
    Not(NotNode)
    Bin(BinNode)
    IsNull(NullNode)
    Call(CallNode)
    // A scalar subquery, `(SELECT ..)`, which must answer one row of one column.
    Sub(SubNode)
    // `EXISTS (SELECT ..)`. `NOT EXISTS` is `Not(Exists(..))` -- `NOT` has its own precedence
    // level now, so it needs no flag of its own here.
    Exists(SubNode)
    InSub(InSubNode)
    Cast(CastNode)
    Like(LikeNode)
    In(InNode)
    Between(BetweenNode)
    Case(CaseNode)
    Star
    // `t.*`, holding the table name or alias it was written with. A separate variant rather than a
    // qualifier on `Star`, so `count(*)` and every other reader of a bare star is untouched.
    TableStar(string)
}

pub struct TableRef {
    // The table's name, or empty for a derived table -- a `(SELECT ..)` in the `FROM`, which has a
    // query instead of a name.
    name: string
    alias: string
    // The query a derived table is. Empty for a named table; one element otherwise. An array
    // rather than an `Option`, the way `CaseNode.fallback` is, so a `TableRef` need not name the
    // type that contains it.
    query: [SelectStmt]
    // `AS x (a, b)` -- the names the derived table's columns take, which may be shorter than the
    // query's list and rename only its first columns. Empty where none were written.
    column_aliases: [string]
}

pub struct Join {
    kind: string
    table: TableRef
    on: Option<Expr>
    using: [string]
}

pub struct SubNode {
    query: SelectStmt
}

pub struct InSubNode {
    subject: Expr
    query: SelectStmt
    negated: bool
}

pub struct CastNode {
    inner: Expr
    type_name: string
}

pub struct SelectItem {
    expr: Expr
    alias: string
}

pub struct OrderTerm {
    expr: Expr
    descending: bool
}

pub struct Assignment {
    column: string
    value: Expr
}

pub struct SetOp {
    kind: string
    // `ALL`, `DISTINCT`, or empty where neither was written -- kept as the word it was rather than
    // reduced to a flag. `DISTINCT` is the default and changes nothing, but SQLite *refuses* it
    // after a set operator where PostgreSQL accepts it, so an engine has to be able to tell
    // `UNION DISTINCT` from `UNION`; a formatter that normalised one into the other would turn SQL
    // one engine rejects into SQL it accepts.
    quantifier: string
    query: SelectStmt
}

pub fn set_op_keeps_all(op: SetOp) -> bool

pub struct SelectStmt {
    distinct: bool
    items: [SelectItem]
    // Empty when the statement has no `FROM`: `SELECT 1` is legal in both real engines and is the
    // liveness probe `datasource`'s pool sends, so it has to be legal here. Otherwise exactly one
    // element, with `joins` carrying the rest.
    from: [TableRef]
    joins: [Join]
    filter: Option<Expr>
    groups: [Expr]
    having: Option<Expr>
    order: [OrderTerm]
    limit: Option<Expr>
    offset: Option<Expr>
    // The set operations applied to this query, left to right. Empty for a simple `SELECT`.
    //
    // On a *compound* query the head carries them, and the head's `order`, `limit` and `offset`
    // belong to the **whole** compound rather than to its first arm -- which is what SQL means by
    // them, since an arm may not have an `ORDER BY` of its own (`SELECT .. ORDER BY .. UNION ..` is
    // an error in both engines). An arm's own `set_ops`, `order`, `limit` and `offset` are always
    // empty, so a reader can treat an arm as a simple query.
    set_ops: [SetOp]
}

pub struct InsertStmt {
    table: string
    columns: [string]
    rows: [[Expr]]
    // `RETURNING a, b` or `RETURNING *`, as select items so an alias and an expression both work.
    // Empty where the statement has no such clause, which is the overwhelming majority.
    returning: [SelectItem]
}

pub struct UpdateStmt {
    table: string
    sets: [Assignment]
    filter: Option<Expr>
    returning: [SelectItem]
}

pub struct DeleteStmt {
    table: string
    filter: Option<Expr>
    returning: [SelectItem]
}

pub struct CreateStmt {
    table: string
    columns: [Column]
    // The table-level constraints -- `PRIMARY KEY (a, b)`, `FOREIGN KEY ... REFERENCES ...`,
    // `UNIQUE (..)`, `CHECK (..)`, each with the name a `CONSTRAINT` prefix gave it.
    constraints: [TableConstraint]
    if_not_exists: bool
}

pub enum AlterAction {
    AddColumn(Column)
    AddConstraint(string)
}

pub struct AlterStmt {
    table: string
    action: AlterAction
}

pub struct CreateIndexStmt {
    name: string
    table: string
    columns: [string]
    unique: bool
    if_not_exists: bool
}

pub struct DropStmt {
    table: string
    if_exists: bool
}

pub enum Statement {
    Select(SelectStmt)
    Insert(InsertStmt)
    Update(UpdateStmt)
    Delete(DeleteStmt)
    Create(CreateStmt)
    CreateIndex(CreateIndexStmt)
    Alter(AlterStmt)
    Drop(DropStmt)
    Begin
    Commit
    Rollback
    // A statement recognised only well enough to pass through -- `PRAGMA`, `SET`, `ANALYZE`,
    // `VACUUM` -- carrying its own text so `format` writes back exactly what was read. Empty for
    // an empty statement.
    Passthrough(string)
}

pub fn parse(sql: string) -> Result<Statement, DbError>

pub fn parse_many(text: string) -> Result<[Statement], DbError>

pub fn parse_file(path: string) -> Result<[Statement], DbError>

pub fn format_value(v: SqlValue) -> string

pub fn format_expr(e: Expr) -> string

pub fn format_constraint(tc: TableConstraint) -> string

pub fn format(s: Statement) -> string

```

## `client`

```
pub enum ClientError {
    BadUrl(string)
    Network(string)
    Timeout
    BadResponse(string)
    TooLarge
    TooManyRedirects
}

pub fn client_error_message(e: ClientError) -> string

pub struct Url {
    secure: bool
    host: string
    port: int
    target: string
    userinfo: string
}

pub fn parse_url(url: string) -> Result<Url, string>

pub fn response_header(r: Response, name: string) -> Option<string>

pub fn close_idle()

pub struct Cookie {
    name: string
    value: string
    domain: string
    path: string
    secure: bool
    host_only: bool
    expires: int
}

pub struct CookieJar {
    cookies: [Cookie]
}

pub fn cookie_jar() -> CookieJar

pub struct JarResponse {
    response: Response
    jar: CookieJar
}

pub fn jar_store(jar: CookieJar, u: Url, headers: [Header]) -> CookieJar

pub fn jar_header(jar: CookieJar, u: Url) -> Option<string>

pub fn request(

pub fn request_with_jar(

pub fn get(url: string) -> Result<Response, ClientError>

pub fn head(url: string) -> Result<Response, ClientError>

pub fn delete(url: string) -> Result<Response, ClientError>

pub fn post_json(url: string, body: string) -> Result<Response, ClientError>

pub fn put_json(url: string, body: string) -> Result<Response, ClientError>

pub fn form_encode(fields: Map<string, string>) -> string

pub fn post_form(url: string, fields: Map<string, string>) -> Result<Response, ClientError>

pub struct StreamResponse {
    status: int
    headers: [Header]
    body: Iter<Result<[byte], ClientError>>
}

pub fn stream(

```

## `derive_routes`

```
pub fn attributes() -> [meta.AttrSpec]

pub fn generate_module(m: meta.ModuleTarget) -> Result<string, string>

```

## `endpoint`

```
pub val application_json = mime.application_json()

pub fn json_decoder<I>(from_json: fn(Json) -> Result<I, string>) -> server.Decoder<I>

pub fn json_encoder<O>(to_json: fn(O) -> Json) -> server.Encoder<O>

pub fn json_value_encoder() -> server.Encoder<Json>

pub fn count_encoder() -> server.Encoder<int>

pub fn list_encoder<O>(to_json: fn(O) -> Json) -> server.Encoder<[O]>

pub fn uuid_param(kind: string) -> fn(server.Params) -> Result<Uuid, string>

pub fn named_uuid_param(name: string, kind: string) -> fn(server.Params) -> Result<Uuid, string>

```

## `http`

```
pub native fn is_token(s: string) -> bool

pub native fn is_field_value(s: string) -> bool

pub val status_continue = 100

pub val status_ok = 200

pub val status_created = 201

pub val status_accepted = 202

pub val status_no_content = 204

pub val status_moved_permanently = 301

pub val status_found = 302

pub val status_not_modified = 304

pub val status_bad_request = 400

pub val status_unauthorized = 401

pub val status_forbidden = 403

pub val status_not_found = 404

pub val status_method_not_allowed = 405

pub val status_not_acceptable = 406

pub val status_request_timeout = 408

pub val status_conflict = 409

pub val status_payload_too_large = 413

pub val status_unsupported_media_type = 415

pub val status_unprocessable_entity = 422

pub val status_too_many_requests = 429

pub val status_header_fields_too_large = 431

pub val status_internal_server_error = 500

pub val status_not_implemented = 501

pub val status_service_unavailable = 503

pub enum HttpMethod {
    Get
    Head
    Post
    Put
    Patch
    Delete
    Options
    Other(string)
}

pub fn method_parse(name: string) -> HttpMethod

pub fn method_name(m: HttpMethod) -> string

pub struct Header {
    name: string
    value: string
}

pub struct Request {
    method: HttpMethod
    target: string
    path: string
    query: string
    version: string
    headers: [Header]
    body: [byte]
    // The address at the other end of the connection this request arrived on, as text ("192.0.2.7" or
    // "2001:db8::1"), with no port. `None` on a Request that did not come off a socket -- one built by
    // hand in a test, or by `write_error`'s stub -- which is the honest answer rather than a placeholder.
    //
    // **The transport peer, and only that.** Not `X-Forwarded-For`: that header is written by whatever
    // is in front, and a client that is not behind a proxy can put anything in it, so a server reading
    // it by default would let every caller choose its own identity -- backwards for the thing an
    // address is wanted for, which is telling one client from another in order to rate-limit (B10). An
    // application behind a proxy it trusts reads the header itself, knowing how many hops it owns;
    // `server.forwarded_for` is there to help once that decision has been made explicitly.
    //
    // A field rather than a synthetic header, deliberately: a header could be spoofed by a client
    // sending that exact name unless the parser stripped it, and relying on that is the same subtlety
    // this field exists to avoid. Where the value comes from should be visible in the type.
    peer: Option<string>
}

pub struct Response {
    status: int
    headers: [Header]
    body: [byte]
}

pub fn response_text(r: Response) -> Result<string, string>

pub fn request_text(req: Request) -> Result<string, string>

pub fn header(req: Request, name: string) -> Option<string>

pub struct TraceParent {
    // 32 lowercase hex characters, the same for every hop of one request.
    trace_id: string
    // 16 lowercase hex characters, this hop's own. A callee makes a child rather than reusing it.
    span_id: string
    // The one flag anything acts on: whether the trace was sampled for recording.
    sampled: bool
}

pub val traceparent_header_name = "traceparent"

pub fn traceparent_parse(value: string) -> Option<TraceParent>

pub fn traceparent_new() -> TraceParent

pub fn traceparent_child(p: TraceParent) -> TraceParent

pub fn traceparent_value(p: TraceParent) -> string

pub fn traceparent_of(req: Request) -> TraceParent

pub fn traceparent_header_for(p: TraceParent) -> Header

pub fn header_value(headers: [Header], name: string) -> Option<string>

pub struct CookiePair {
    name: string
    value: string
}

pub fn request_cookies(req: Request) -> [CookiePair]

pub fn request_cookie(req: Request, name: string) -> Option<string>

pub struct SetCookie {
    name: string
    value: string
    path: string
    domain: string
    max_age_seconds: int
    secure: bool
    http_only: bool
    same_site: string
}

pub fn session_cookie(name: string, value: string) -> SetCookie

pub fn set_cookie_header(c: SetCookie) -> Header

pub fn delete_cookie_header(name: string, path: string, domain: string) -> Header

pub fn content_type(headers: [Header]) -> Option<mime.MediaType>

pub fn quality(headers: [Header], media: mime.MediaType) -> float

pub fn accepts(headers: [Header], media: mime.MediaType) -> bool

pub fn best_media(headers: [Header], candidates: [mime.MediaType]) -> Option<mime.MediaType>

pub fn percent_decode(s: string, plus_is_space: bool) -> Result<string, string>

pub fn percent_encode(s: string, plus_for_space: bool) -> string

```

## `server`

```
pub struct Params {
    names: [string]
    values: [string]
}

pub enum HttpError {
    BadRequest(string)
    BodyTooLarge
    HeadersTooLarge
    NotImplemented(string)
    Timeout
    Closed
    Io(string)
}

pub struct Session {
    sock: Socket
    buffer: [byte]
    timeout_ms: int
    head_timeout_ms: int
    fresh: bool
    // The largest request body `read_request` will accept, or 0 for this module's 1 MiB default.
    // `serve` fills it from `ServeOptions.max_body_bytes`; the `open_session*` constructors leave it
    // 0, so a caller reading requests off its own socket keeps the default until it says otherwise.
    //
    // It is here rather than only on `ServeOptions` because `read_request(session)` is the public
    // entry point and the North Star backend calls it directly -- a limit the server knows has to
    // reach it somehow, and widening that signature would break every such caller (D21, D22).
    max_body_bytes: int
}

pub struct Exchange {
    request: Request
    session: Session
}

pub fn open_session(sock: Socket) -> Session

pub fn open_session_with_timeout(sock: Socket, timeout_ms: int) -> Session

pub fn open_session_with_timeouts(sock: Socket, timeout_ms: int, head_timeout_ms: int) -> Session

pub fn read_request(session: Session) -> Result<Exchange, HttpError>

pub fn response(status: int, body: [byte]) -> Response

pub fn with_header(r: Response, name: string, value: string) -> Response

pub fn media_response(status: int, body: [byte], media: mime.MediaType) -> Response

pub fn json_response(status: int, body: string) -> Response

pub fn text_response(status: int, body: string) -> Response

pub fn bad_request(message: string) -> Response

pub fn not_found() -> Response

pub fn method_not_allowed(allowed: variadic string) -> Response

pub fn error_response(e: HttpError) -> Response

pub fn error_status(e: HttpError) -> int

pub fn error_text(e: HttpError) -> string

pub fn write_response(sock: Socket, req: Request, response: Response) -> Result<bool, HttpError>

pub fn compressed(req: Request, resp: Response, min_bytes: int) -> Response

pub fn write_error(sock: Socket, e: HttpError) -> Result<bool, HttpError>

pub fn write_error_with(

pub fn path_params(pattern: string, path: string) -> Option<Params>

pub fn route(req: Request, method: HttpMethod, pattern: string) -> Option<Params>

pub fn param(p: Params, name: string) -> string

pub fn not_found_or_method_not_allowed(req: Request, routes: [string]) -> Response

pub fn query_param(req: Request, name: string) -> Option<string>

pub struct Part {
    name: string
    filename: Option<string>
    content_type: Option<string>
    body: [byte]
}

pub val multipart_max_parts = 128

pub fn multipart_boundary(req: Request) -> Option<string>

pub fn multipart(req: Request) -> Result<[Part], HttpError>

pub fn multipart_with(req: Request, max_parts: int) -> Result<[Part], HttpError>

pub fn part(parts: [Part], name: string) -> Option<Part>

pub fn part_text(parts: [Part], name: string) -> Result<Option<string>, string>

pub struct Endpoint<C> {
    method: HttpMethod
    pattern: string
    handler: fn(C, Request, Params) -> Response
}

pub struct Decoder<I> {
    media: mime.MediaType
    decode: fn([byte]) -> Result<I, string>
}

pub struct Encoder<O> {
    media: mime.MediaType
    encode: fn(O) -> [byte]
}

pub fn decoder<I>(media: mime.MediaType, decode: fn([byte]) -> Result<I, string>) -> Decoder<I>

pub fn text_decoder<I>(media: mime.MediaType, decode: fn(string) -> Result<I, string>) -> Decoder<I>

pub fn encoder<O>(media: mime.MediaType, encode: fn(O) -> [byte]) -> Encoder<O>

pub fn json_decoder<I>(from_json: fn(Json) -> Result<I, string>) -> Decoder<I>

pub fn json_encoder<O>(to_json: fn(O) -> Json) -> Encoder<O>

pub fn json_list_encoder<O>(to_json: fn(O) -> Json) -> Encoder<[O]>

pub fn text_encoder<O>(media: mime.MediaType, encode: fn(O) -> string) -> Encoder<O>

pub enum Denied {
    Unauthenticated(string)
    Forbidden(string)
    Failed(string)
}

pub struct Api<E> {
    failure: fn(E) -> Response
    fault: fn(int, string) -> Response
}

pub fn secure_handler<C, U, O, E>(

pub fn secure_path_handler<C, U, P, O, E>(

pub fn secure_query_handler<C, U, Q, O, E>(

pub fn secure_body_handler<C, U, I, O, E>(

pub fn secure_path_body_handler<C, U, P, I, O, E>(

pub fn public_handler<C, O, E>(

pub fn public_path_handler<C, P, O, E>(

pub fn public_body_handler<C, I, O, E>(

pub fn public_path_body_handler<C, P, I, O, E>(

pub struct Routes<C> {
    endpoints: [Endpoint<C>]
    patterns: [Pattern]
    by_method: Map<string, [int]>
    by_length: Map<int, [int]>
    // Whether `dispatch` records a metric per request. Set by `metered_routes`, not by `routes`:
    // the matched pattern is known here and nowhere else, which is the one label worth having and
    // the reason this lives on the table rather than in `ServeOptions`.
    meter: bool
}

pub fn rate_allow(bucket: string, key: string, limit: int, window_seconds: int) -> bool

pub fn rate_allow_at(bucket: string, key: string, limit: int, window_seconds: int, now_seconds: int) -> bool

pub fn rate_reset()

pub fn open_connections_of(address: string) -> int

pub fn forwarded_for(req: Request, trusted_hops: int) -> Option<string>

pub fn declare_metrics()

pub fn metrics_response() -> Response

pub fn metrics_handler<C>(ctx: C, req: Request, params: Params) -> Response

pub fn routes<C>(endpoints: [Endpoint<C>]) -> Routes<C>

pub fn metered_routes<C>(endpoints: [Endpoint<C>]) -> Routes<C>

pub struct CorsPolicy {
    allowed_origins: [string]
    allowed_methods: string
    allowed_headers: string
    max_age_seconds: int
    allow_credentials: bool
}

pub fn cors_policy(origins: [string]) -> CorsPolicy

pub fn cors_preflight(policy: CorsPolicy, req: Request, fault: fn(int, string) -> Response) -> Option<Response>

pub fn cors_apply(policy: CorsPolicy, req: Request, answer: Response) -> Response

pub fn dispatch<C>(table: Routes<C>, ctx: C, req: Request, fault: fn(int, string) -> Response) -> Response

pub struct RequestLog {
    method: HttpMethod
    target: string
    status: int
    duration_ms: int
    response_bytes: int
    peer: Option<string>
}

pub struct ServeOptions {
    idle_timeout_ms: int
    header_timeout_ms: int
    // The most requests served at once -- not the most clients connected. A connection holds one
    // of these only while it is actually serving a request; between requests it is parked with the
    // poller and holds nothing, so a server can hold far more keep-alive clients than this number
    // (`net.wait_readable`, and `park_loop` below).
    //
    // It used to mean the other thing, and the difference is worth knowing if you are carrying a
    // size forward: a connection held a slot for its whole life, so under keep-alive an occupant
    // kept one through `idle_timeout_ms` of doing nothing and a 65th client against a cap of 64
    // waited for a *timeout* rather than for a request. 256 keep-alive clients against a cap of 64
    // took one full idle timeout whatever the request count -- 31.5s -- where they now take
    // 1.5-2.0s and a cap of 64 measures the same as a cap of 256 (docs/reviews/tasks/ F139).
    //
    // So size it by the concurrency of *work* you want to admit, which is what a thread-per-request
    // server can usefully run at once, and let the client count follow from sockets.
    max_connections: int
    log_requests: bool
    // The largest request body to accept, or 0 for the library's 1 MiB default. A body over it is
    // refused with 413 before the handler is reached, and the limit applies to what arrives on the
    // wire -- the encoded body, `multipart` boundaries included.
    max_body_bytes: int
    // How to render a rejection the server makes before the handler is ever reached -- a 400 for a malformed
    // request line, a 408 when the client stops mid-request, a 413 for too large a body, a 431 for too large a
    // header block, a 501 for a method the parser will not take. `None` uses the library's own JSON shape.
    //
    // Pass the same renderer the application gives `dispatch`, and a client sees one error shape from the whole
    // server instead of two: a browser that cannot read the body of its own 413 because it arrived in a
    // different shape with none of the application's headers is the reason this exists. Whatever it returns is
    // written as-is, so an HSTS header it adds is on the rejection too.
    //
    // It gets a status and a sentence, not the Request: these failures happen while the request is being read,
    // so there may be no parsed request to pass -- which is also why a per-request header like
    // `Access-Control-Allow-Origin` cannot be set here, there being no `Origin` to answer.
    fault: Option<fn(int, string) -> Response>
    // Called once per finished exchange, with everything the server knows about it. This is the
    // composable half of request logging: `log_requests` gives the library's one fixed line, and
    // this gives the application the fields to write its own -- through the `log` module, as JSON,
    // with a request id, or wherever its logs actually go. The two are independent; setting both
    // prints the built-in line *and* calls this.
    //
    // A field on the options, not a wrapper around `dispatch` as the roadmap first imagined: a
    // wrapper would have to return a closure and allocate one per request, which is the same reason
    // the CORS pair is two plain functions (tasks.md F59). One function value is stored here once.
    //
    // **Only completed exchanges.** A request the server rejects before parsing one -- a malformed
    // request line, a client that stops mid-head, a body over the cap -- never reaches this, because
    // there is no request to describe; the same reason `fault` above takes a status and a sentence
    // rather than a `Request`. Those rejections are `fault`'s to render.
    //
    // Typed `fn(RequestLog)` and not `fn(RequestLog) -> unit`: a void function's type *is* the
    // former, so an application writes `fn note(entry: RequestLog) { ... }` and passes it, with no
    // `-> unit` to remember. This is the library's first callback that returns nothing, so the
    // spelling is set here.
    on_request: Option<fn(RequestLog)>
    // Whether to record `http_open_connections` and the two byte counters. Per-request metrics are
    // separate, on the route table (`metered_routes`), because only something that matched the request
    // knows the pattern to label it with -- and a pattern is the only label worth having.
    metrics: bool
    // Gzip a response when the client offers it and the body is at least 1 KiB -- see `compressed`,
    // which this simply turns on. Off by default on purpose: compressing a response that carries a
    // secret alongside attacker-influenced text leaks it through the length (BREACH), and an API
    // whose login answer holds a JWT is that shape. Ask for it per server rather than inheriting it.
    compress_responses: bool
    // How many connections one address may hold at once. 0 is off, and is the default a server should keep
    // unless it is reached directly by clients: behind a reverse proxy every connection carries the
    // proxy's address, so any cap would be a cap on the proxy.
    //
    // This is the half a request throttle cannot cover. A client that opens connections and never sends a
    // request is counted by no rate limit, and can fill every `max_connections` slot;
    // `header_timeout_ms` frees such a slot within seconds, which bounds how long the flood holds one, not
    // how many it holds at once. A refusal here closes the socket without writing anything -- answering
    // would mean writing to a flooding client from the accept loop, where one slow read stalls every other
    // connection's accept.
    //
    // The address is the socket's, necessarily: at accept time there is no request yet, so there is no
    // `X-Forwarded-For` to consult even if it were trustworthy. That is the same reason the default is off.
    max_connections_per_client: int
}

pub fn serve_options() -> ServeOptions

pub fn serve(listener: Listener, handler: fn(Request) -> Response, options: ServeOptions)

```

## `schedule`

```
pub enum Schedule {
    // Start a run every N milliseconds, the first one immediately.
    EveryMillis(int)
    // Run once a day, at this many seconds past 00:00:00 UTC.
    DailyAtUtc(int)
    // Run whenever the UTC wall clock matches a cron pattern (`cron`).
    Cron(CronSpec)
}

pub struct CronSpec {
    seconds: [bool]
    minutes: [bool]
    hours: [bool]
    days_of_month: [bool]
    months: [bool]
    days_of_week: [bool]
    dom_restricted: bool
    dow_restricted: bool
}

pub fn every_millis(ms: int) -> Result<Schedule, string>

pub fn daily_at_utc(hour: int, minute: int) -> Result<Schedule, string>

pub fn cron(expr: string) -> Result<Schedule, string>

pub struct Job {
    name: string
    schedule: Schedule
    run: fn() -> Result<unit, string>
}

pub fn job(name: string, schedule: Schedule, run: fn() -> Result<unit, string>) -> Job

pub fn run_until(jobs: [Job], stop: Channel<unit>, on_error: fn(string, string))

pub fn cron_matches_at(spec: CronSpec, epoch_second: int) -> bool

```

## `time`

Seven `java.time`-shaped types (`LocalDate`, `LocalTime`, `LocalDateTime`, `Duration`, `Instant`, `ZoneOffset`, `ZoneId`), written in pure Ziman on top of `clock`. `ZoneId` reads the IANA zone files (see `tzdata`). Every type has a fixed ISO-8601 `_to_string`/`_parse` pair; `LocalDate`/`LocalTime`/`LocalDateTime`/`Instant` also get a general `_format`/`_parse_pattern` pair taking a `DateTimeFormat` -- a pattern compiled once by `datetime_format_of` (see the `iso_*_format` vals, each compiled exactly once at program start) rather than a raw pattern string re-scanned on every call.

```
pub fn is_leap_year(year: int) -> bool

pub struct DateTimeFormat {
    parts: [DateTimePart]
    width: int
}

pub fn datetime_format_of(pattern: string) -> DateTimeFormat

pub val iso_date_format = datetime_format_of("yyyy-MM-dd")

pub val iso_time_format = datetime_format_of("HH:mm:ss")

pub val iso_time_nanos_format = datetime_format_of("HH:mm:ss.SSSSSSSSS")

pub val iso_datetime_format = datetime_format_of("yyyy-MM-ddTHH:mm:ss")

pub val iso_datetime_nanos_format = datetime_format_of("yyyy-MM-ddTHH:mm:ss.SSSSSSSSS")

pub struct LocalDate {
    year: int
    month: int
    day: int
}

pub fn localdate_of(year: int, month: int, day: int) -> Result<LocalDate, string>

pub fn to_epoch_day(d: LocalDate) -> int

pub fn localdate_from_epoch_day(epoch_day: int) -> LocalDate

pub fn add_days(d: LocalDate, days: int) -> LocalDate

pub fn add_months(d: LocalDate, months: int) -> LocalDate

pub fn add_years(d: LocalDate, years: int) -> LocalDate

pub fn diff_days(a: LocalDate, b: LocalDate) -> int

pub fn localdate_compare(a: LocalDate, b: LocalDate) -> int

pub fn format(d: LocalDate, fmt: DateTimeFormat) -> string

pub fn format(d: LocalDate, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_localdate(s: string, fmt: DateTimeFormat) -> Result<LocalDate, string>

pub fn parse_localdate(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalDate, string>

pub fn parse_localdate(s: string) -> Result<LocalDate, string>

pub fn format(d: LocalDate) -> string

pub struct LocalTime {
    hour: int
    minute: int
    second: int
    nano: int
}

pub fn localtime_of(hour: int, minute: int, second: int, nano: int) -> Result<LocalTime, string>

pub fn add_nanos(t: LocalTime, nanos: int) -> LocalTime

pub fn add_seconds(t: LocalTime, seconds: int) -> LocalTime

pub fn add_minutes(t: LocalTime, minutes: int) -> LocalTime

pub fn add_hours(t: LocalTime, hours: int) -> LocalTime

pub fn diff_nanos(a: LocalTime, b: LocalTime) -> int

pub fn localtime_compare(a: LocalTime, b: LocalTime) -> int

pub fn format(t: LocalTime, fmt: DateTimeFormat) -> string

pub fn format(t: LocalTime, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_time(s: string, fmt: DateTimeFormat) -> Result<LocalTime, string>

pub fn parse_time(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalTime, string>

pub fn parse_time(s: string) -> Result<LocalTime, string>

pub fn format(t: LocalTime) -> string

pub struct Duration {
    seconds: int
    nanos: int
}

pub fn duration_of_seconds(seconds: int, nanos: int) -> Duration

pub fn duration_zero() -> Duration

pub fn add(a: Duration, b: Duration) -> Duration

pub fn negate(d: Duration) -> Duration

pub fn subtract(a: Duration, b: Duration) -> Duration

pub fn duration_compare(a: Duration, b: Duration) -> int

pub fn format(d: Duration) -> string

pub fn parse_duration(s: string) -> Result<Duration, string>

pub struct LocalDateTime {
    date: LocalDate
    time: LocalTime
}

pub fn localdatetime_of(date: LocalDate, time: LocalTime) -> LocalDateTime

pub fn add_nanos(dt: LocalDateTime, nanos: int) -> LocalDateTime

pub fn add_seconds(dt: LocalDateTime, seconds: int) -> LocalDateTime

pub fn add_minutes(dt: LocalDateTime, minutes: int) -> LocalDateTime

pub fn add_hours(dt: LocalDateTime, hours: int) -> LocalDateTime

pub fn add_days(dt: LocalDateTime, days: int) -> LocalDateTime

pub fn add_months(dt: LocalDateTime, months: int) -> LocalDateTime

pub fn add_years(dt: LocalDateTime, years: int) -> LocalDateTime

pub fn localdatetime_compare(a: LocalDateTime, b: LocalDateTime) -> int

pub fn diff(a: LocalDateTime, b: LocalDateTime) -> Duration

pub fn format(dt: LocalDateTime, fmt: DateTimeFormat) -> string

pub fn format(dt: LocalDateTime, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_datetime(s: string, fmt: DateTimeFormat) -> Result<LocalDateTime, string>

pub fn parse_datetime(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalDateTime, string>

pub fn parse_datetime(s: string) -> Result<LocalDateTime, string>

pub fn format(dt: LocalDateTime) -> string

pub struct ZoneOffset {
    total_seconds: int
}

pub fn zoneoffset_utc() -> ZoneOffset

pub fn zoneoffset_of_seconds(total_seconds: int) -> Result<ZoneOffset, string>

pub fn zoneoffset_of_hours_minutes(hours: int, minutes: int) -> Result<ZoneOffset, string>

pub fn zoneoffset_compare(a: ZoneOffset, b: ZoneOffset) -> int

pub fn format(z: ZoneOffset) -> string

pub fn parse_offset(s: string) -> Result<ZoneOffset, string>

pub struct ZoneId {
    name: string
    rules: ZoneRules
}

pub fn zoneid_utc() -> ZoneId

pub fn zoneid_of(name: string) -> Result<ZoneId, string>

pub fn zoneid_offset_at(z: ZoneId, i: Instant) -> ZoneOffset

pub fn zoneid_is_dst_at(z: ZoneId, i: Instant) -> bool

pub fn zoneid_abbreviation_at(z: ZoneId, i: Instant) -> string

pub fn format(z: ZoneId) -> string

pub fn parse_zone(s: string) -> Result<ZoneId, string>

pub struct Instant {
    seconds: int
    nanos: int
}

pub fn instant_of(seconds: int, nanos: int) -> Instant

pub fn instant_now() -> Instant

pub fn add(i: Instant, d: Duration) -> Instant

pub fn subtract(i: Instant, d: Duration) -> Instant

pub fn diff(a: Instant, b: Instant) -> Duration

pub fn instant_compare(a: Instant, b: Instant) -> int

pub fn to_localdatetime(i: Instant, offset: ZoneOffset) -> LocalDateTime

pub fn to_instant(dt: LocalDateTime, offset: ZoneOffset) -> Instant

pub fn localdatetime_now(offset: ZoneOffset) -> LocalDateTime

pub fn localdate_now(offset: ZoneOffset) -> LocalDate

pub fn localtime_now(offset: ZoneOffset) -> LocalTime

pub fn to_localdatetime(i: Instant, z: ZoneId) -> LocalDateTime

pub fn to_instant(dt: LocalDateTime, z: ZoneId) -> Instant

pub fn format(i: Instant, offset: ZoneOffset, fmt: DateTimeFormat) -> string

pub fn format(i: Instant, offset: ZoneOffset, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_instant(s: string, fmt: DateTimeFormat) -> Result<Instant, string>

pub fn parse_instant(s: string, fmt: DateTimeFormat, l: Locale) -> Result<Instant, string>

pub fn parse_instant(s: string) -> Result<Instant, string>

pub fn format(i: Instant) -> string

pub fn to_json(d: LocalDate) -> Json

pub fn to_localdate(j: Json) -> Result<LocalDate, string>

pub fn to_json(t: LocalTime) -> Json

pub fn to_localtime(j: Json) -> Result<LocalTime, string>

pub fn to_json(d: Duration) -> Json

pub fn to_duration(j: Json) -> Result<Duration, string>

pub fn to_json(dt: LocalDateTime) -> Json

pub fn to_localdatetime(j: Json) -> Result<LocalDateTime, string>

pub fn to_json(z: ZoneOffset) -> Json

pub fn to_zoneoffset(j: Json) -> Result<ZoneOffset, string>

pub fn to_json(z: ZoneId) -> Json

pub fn to_zoneid(j: Json) -> Result<ZoneId, string>

pub fn to_json(i: Instant) -> Json

pub fn to_instant(j: Json) -> Result<Instant, string>

pub fn to_sql(v: LocalDate) -> SqlValue

pub fn to_localdate(v: SqlValue) -> Result<LocalDate, DbError>

pub fn to_sql(v: LocalTime) -> SqlValue

pub fn to_localtime(v: SqlValue) -> Result<LocalTime, DbError>

pub fn to_sql(v: Duration) -> SqlValue

pub fn to_duration(v: SqlValue) -> Result<Duration, DbError>

pub fn to_sql(v: LocalDateTime) -> SqlValue

pub fn to_localdatetime(v: SqlValue) -> Result<LocalDateTime, DbError>

pub fn to_sql(v: ZoneOffset) -> SqlValue

pub fn to_zoneoffset(v: SqlValue) -> Result<ZoneOffset, DbError>

pub fn to_sql(v: ZoneId) -> SqlValue

pub fn to_zoneid(v: SqlValue) -> Result<ZoneId, DbError>

pub fn to_sql(v: Instant) -> SqlValue

pub fn to_instant(v: SqlValue) -> Result<Instant, DbError>

```

## `zone`

The IANA time zone database, read from the operating system's zone files, for `time`'s named zones (`zoneid_of`).

```
pub struct ZoneType {
    utoff: int
    dst: bool
    name: string
}

pub struct RuleDate {
    kind: int
    month: int
    week: int
    weekday: int
    day: int
    seconds: int
}

pub struct PosixRule {
    standard: ZoneType
    daylight: ZoneType
    has_dst: bool
    start: RuleDate
    end: RuleDate
}

pub struct ZoneRules {
    transitions: [int]
    type_of: [int]
    utoffs: [int]
    dsts: [bool]
    names: [string]
    has_rule: bool
    rule: PosixRule
}

pub fn parse_posix_rule(text: string) -> Result<PosixRule, string>

pub val no_rule = PosixRule {

pub fn parse_rules(data: [byte]) -> Result<ZoneRules, string>

pub fn load_rules(name: string) -> Result<ZoneRules, string>

pub fn type_at(rules: ZoneRules, t: int) -> ZoneType

```
