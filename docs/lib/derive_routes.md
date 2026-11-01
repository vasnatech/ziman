# `derive_routes`

`#![derive(Routes)]` on a module, and `#[route(..)]` on the functions it routes to: the endpoint
table written from the handlers' own signatures.


```
#![derive(Routes)]

#[route(method: POST, path: "/api/orders", status: 201)]
pub fn handle_create(ctx: Ctx, who: Principal, data: model.OrderRequest)
    -> Result<model.OrderResponse, ApiError> { .. }
```

writes `pub val <module>_routes: [server.Endpoint<Ctx>]`, which the application appends to its
own table. One attribute per handler replaces about ten lines of `server.Endpoint` literal.

**Almost all of it comes from the signature, not the attribute.** Whether the endpoint is
secured is whether its second parameter is a `Principal`; whether its input is in the path or
the body is whether it takes a `Uuid` or a struct; and those three facts choose one of
`server`'s eight handler constructors. The answer's encoder comes from the return type -- a
`[T]` takes `json_list_encoder`, a `unit` takes none at all, a `Json` and an `int` take
`codec`'s two builders that hold no converter (`json_value_encoder()` and `count_encoder()`,
the second being what a count endpoint answers), anything else takes `json_encoder`. The decoder and encoder *functions* are named from the types as the
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

**What the application must have in scope where this is spliced**: `Ctx` (its context type),
`api` (its `server.Api`, which says how its own error type renders), the `who` function each
route names, and the four names below from `codec` and `server` -- `json_decoder`,
`json_encoder`, `json_list_encoder`, `uuid_param`/`named_uuid_param`. They are emitted bare rather than qualified
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
