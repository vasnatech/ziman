# `dburl`

Parsing of database URLs shared by the network drivers
(`pg`, `mysql`): `scheme://user[:password]@host[:port]/database[?k=v&...]`.
User, password, database and query values are percent-decoded, so a
password may contain `@`, `:` or `/` when written as %40, %3A, %2F (a
literal `@` in the password also works: the LAST `@` separates userinfo
from host).

`use dburl` -- or `use dburl.Name` to bring one item in bare.

### `UrlParam`

```
pub struct UrlParam {
    name: string
    value: string
}
```

One `name=value` pair from the query part of a database URL, already decoded.

### `DbUrl`

```
pub struct DbUrl {
    user: string
    password: string
    host: string
    port: int
    database: string
    params: [UrlParam]
}
```

A parsed database URL: the decoded credentials, host, port (0 when the URL names none), database
and query parameters.

### `percent_decode`

```
pub fn percent_decode(s: string) -> Result<string, DbError>
```

Decodes `%XX` escapes in `s`; an Err for a malformed escape or one that yields a NUL byte.

### `param`

```
pub fn param(u: DbUrl, name: string) -> Option<string>
```

The query parameter `name`, if the URL has it.

### `parse`

```
pub fn parse(url: string, prefixes: [string], default_port: int) -> Result<DbUrl, DbError>
```

`prefixes` are the accepted `scheme://` starts (e.g. "postgres://").
The database defaults to the user name when the URL has none.
