# `paging`

One page of a long list: the request that asks for a page, the page that comes back, and the
either/or a list endpoint answers -- all of it, or the page that was asked for.

**This is serialization, not HTTP**, which is the same reason `codec` is here rather than under
`lib/http/`: a page is a window onto a list, `page_of` is arithmetic, and `listing_to_json`
writes a tree. Nothing below mentions a request, a response or a status. What *is* HTTP is
reading `?page=&size=` off a query string and answering 400 for a bad one, and that stays with
whoever serves the request -- `page_request_of` takes the two values already bound and hands
back a message for the refusal rather than making one.

**The shape is Spring's `Page`**, because a client that already reads one should not have to
learn a second: `{"content": [..], "page": 0, "size": 20, "totalElements": 1234,
"totalPages": 62}`, the four counters beside the items rather than wrapped in a `pageable`
object. Without `page` and without `size` the list is answered whole, as a bare array, so a
client written before paging existed sees no difference; with either, the other defaults and the
answer is a page.

**Extracted from the North Star backend on 2026-11-01**, where it was `model/paging.zm`. Its
only imports were `json`'s -- nothing of that application's -- which is what said it was a
library module shelved in an application: paging is what every list endpoint does, and the one
thing in it that *was* policy (how big a page may be) is now an argument.

`use paging` -- or `use paging.Name` to bring one item in bare.

### `default_page_size`

```
pub val default_page_size = 20
```

What a page is when a request asks for one without saying how big, and the largest it may ask
for. Spring's own defaults are 20 and 2000 (`spring.data.web.pageable.*`); the cap here is
lower because a page is held in memory to be encoded, and an API that wants another passes it
to `page_request_within`.

### `max_page_size`

```
pub val max_page_size = 200
```

### `max_page_number`

```
pub val max_page_number = 1000000000
```

The highest page number a request may name. Not a limit on the data -- it is a limit on the
arithmetic: `page * size` is computed to skip that many rows, and a billion pages of twenty is
already past anything a client can be asking for honestly.

### `PageRequest`

```
pub struct PageRequest {
    page: int
    size: int
}
```

Which page of the list was asked for: the first is 0.

### `Page`

```
pub struct Page<T> {
    content: [T]
    page: int
    size: int
    total_elements: int
    total_pages: int
}
```

One page of a list, with how many items and pages there are in all.

### `Listing`

```
pub enum Listing<T> {
    All([T])
    Paged(Page<T>)
}
```

What a list endpoint answers: all of it, or the page that was asked for.

### `page_request_within`

```
pub fn page_request_within(
    page: Option<int>,
    size: Option<int>,
    default_size: int,
    max_size: int
) -> Result<Option<PageRequest>, string>
```

The `?page=&size=` of a list request, within the bounds this API allows: nothing when neither
was sent (the list is answered whole), otherwise the page asked for. An `Err` message is for
whoever asked, and an HTTP server answers it 400.

**It takes the two bound values rather than the request**, because `#[query] page: Option<int>`
is what a route declares and the one thing a per-parameter binding cannot express is a rule
*between* two parameters -- which is exactly what "neither was sent" is.

### `page_request_of`

```
pub fn page_request_of(page: Option<int>, size: Option<int>) -> Result<Option<PageRequest>, string>
```

The same, with this module's defaults: a page of `default_page_size` items, at most
`max_page_size`.

### `page_of`

```
pub fn page_of<T>(content: [T], request: PageRequest, total: int) -> Page<T>
```

`content` as the page `request` asked for of a list with `total` items.

### `listing_to_json`

```
pub fn listing_to_json<T>(listing: Listing<T>, item: fn(T) -> Json) -> Json
```

The JSON of a listing: an array of the items, or the page object; `item` writes one item.

### `listing_encoder`

```
pub fn listing_encoder<O>(to_json: fn(O) -> Json) -> codec.Encoder<Listing<O>>
```

A listing as JSON, as a `codec.Encoder` an endpoint can answer with -- built from the item's own
derived `<snake>_to_json`, like `codec.json_encoder` is for a lone value.

**JSON and nothing else**, where `codec.encoder_of` takes any `TreeFormat`: `Listing` is a
wrapper this module writes the tree for by hand, and `content`/`totalElements` as CSV columns or
as a YAML document is a shape nobody asked for. An API that wants its page in another format
writes the envelope it wants and encodes that.

`#![derive(Routes)]` emits a call to this for any route answering `Result<Listing<T>, E>`, so a
controller names it in its imports and nowhere else.
