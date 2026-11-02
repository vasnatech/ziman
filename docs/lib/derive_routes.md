# `derive_routes`

`#![derive(Routes)]` on a module, and `#[route(..)]` on the functions it routes to: the endpoint
table written from the handlers' own signatures.


```
#![derive(Routes)]

#[route(method: POST, path: "/api/orders", status: 201)]
pub fn handle_create(ctx: Ctx, who: Principal, data: model.OrderRequest)
    -> Result<model.OrderResponse, ApiError> { .. }
```

writes `pub val <module>_routes: [rest.Endpoint<Ctx>]`, which the application appends to its
own table. One attribute per handler replaces about ten lines of `rest.Endpoint` literal.

**Almost all of it comes from the signature, not the attribute.** Whether the endpoint is
secured is whether its second parameter is a `Principal`; whether its input is in the path or
the body is whether it takes a `Uuid`, a struct or a `server.Part`; and those three facts choose
one of `rest`'s sixteen handler constructors. The answer's encoder comes from the return type -- a
`[T]` takes `json_list_encoder`, a `unit` takes none at all, a `Json` and an `int` take
`codec`'s two builders that hold no converter (`json_value_encoder()` and `count_encoder()`,
the second being what a count endpoint answers), a `Listing<T>` takes `paging`'s
`listing_encoder`, anything else takes `json_encoder`. The decoder and encoder *functions* are named from the types as the
source spells them: `meta` carries `Named("model.OrderRequest")`, alias and all, so
`model.order_request_from_json` is `#[derive(Json)]`'s own convention read backwards.

**Four things no signature holds, so the attribute carries them:**


```
* `method:` and `path:`, which are its whole content.
* `consumes:` and `produces:`, Spring's words, naming `codec`'s formats -- `"json"`, `"yaml"`,
  `"toml"`, `"ndjson"`, `"csv"`, `"xml"`, or `"all"`. Omitted means JSON. Which of them a
  shape can carry *is* derivable and is checked here, so `produces: "toml"` on a list is
  refused at the attribute rather than on the request that asks for it.

  **`"xml"` is spelled like the others and reached differently**, because XML is not a
  `codec.TreeFormat`: `xml.parse` answers a `Document`, with attributes, namespaces and
  element order that no `Json` carries, so there is no `xml_format()` to pass. What makes it
  nameable here is `#[derive(Xml)]` (2026-10-31), which gives a type a `_to_xml`/`_from_xml`
  pair the way `#[derive(Json)]` gives it a `_to_json`/`_from_json` one -- so this emits
  `xml_encoder(model.order_to_xml)` where a tree format gets
  `encoder_of(json_format(), model.order_to_json)`. A model named with `"xml"` has to derive
  `Xml` as well as `Json`; if it does not, the generated call does not resolve and says so.

  **`"all"` does not include XML**, deliberately: it means every format the type's JSON
  derive already provides, so adding `produces: "all"` never demands a second derive. XML is
  named explicitly.
* `who:`, because two authorisation functions have the same signature. Defaults to
  `authenticated`.
* `status:`, because 201-versus-200 on a create is a decision, not a type.
* `label:`, the noun in a bad-id message ("invalid order id"), defaulting to the module's own
  name with underscores as spaces -- which is what an application names them anyway.
```

**An uploaded file is the one input that is not decoded.** `#[part(name: "file")]` -- Spring's
`@RequestPart` -- binds one part of a `multipart/form-data` body as a `server.Part`: bytes, a
filename, and the client's claim about its own media type. It picks `rest`'s `*_form_handler`
family where a decoded body picks `*_body_handler`, and the table names `rest.named_part("file")`
where that one lists its `decoders:`.


```
* **A `server.Part` parameter needs no attribute**, as a `Uuid` needs no `#[path]`: a part is
  the only thing that type is bound from. The attribute is how a form field named something
  other than the parameter is said.
* **`Option<server.Part>` is a part the form need not carry**, and takes `optional_part`. A
  malformed body is still a 400 either way; only the part's absence becomes `None`.
* **`#[part]` on a scalar is a form *field*, not a file** -- `@RequestParam("unit") String` on
  a multipart request, which reads the body rather than the query string. It is text on the
  wire like a query parameter, so it takes the same conversions and the same `default:`:
  `#[part(default: "mg/dL")] unit: string`, `#[part] rows: Option<int>`.
* **Several `#[part]`s are a holder struct, a generated reader and an adapter**, exactly as
  several `#[query]` parameters are: the body is read once into `parts` and each binding taken
  out of it. A glucometer import -- one file and four fields -- is the shape that asked for it.
* **`consumes:` has nothing to say about one** and saying it anyway is refused, which is the
  same fact as a form not being a `codec.Decoder`: a decoder is `[byte] -> T` and the boundary
  a multipart body is split on is in the `Content-Type` header, which it never sees.
```

**What the application must have in scope where this is spliced**: `Ctx` (its context type),
`api` (its `rest.Api`, which says how its own error type renders), the `who` function each
route names, and the four names below from `codec` and `server` -- `json_decoder`,
`json_encoder`, `json_list_encoder`, `uuid_param`/`named_uuid_param` -- plus
`paging.listing_encoder` for a route answering a `Listing`. They are emitted bare rather than qualified
so that an application is free to bring them in by per-name import, and so that this module
invents no vocabulary for an application's error type (user-generators.md §5c).

`use derive_routes` -- or `use derive_routes.Name` to bring one item in bare.

### `attributes`

```
pub fn attributes() -> [meta.AttrSpec]
```

### `generate_module`

```
pub fn generate_module(m: meta.ModuleTarget) -> Result<string, string>
```
