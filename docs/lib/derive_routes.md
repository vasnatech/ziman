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
`[T]` takes `json_list_encoder`, a `unit` takes none at all, anything else takes
`json_encoder` -- `codec`'s. The decoder and encoder *functions* are named from the types as the
source spells them: `meta` carries `Named("model.OrderRequest")`, alias and all, so
`model.order_request_from_json` is `#[derive(Json)]`'s own convention read backwards.

**Four things no signature holds, so the attribute carries them:**


```
* `method:` and `path:`, which are its whole content.
* `who:`, because two authorisation functions have the same signature. Defaults to
  `authenticated`.
* `status:`, because 201-versus-200 on a create is a decision, not a type.
* `label:`, the noun in a bad-id message ("invalid order id"), defaulting to the module's own
  name with underscores as spaces -- which is what an application names them anyway.
```

**What the application must have in scope where this is spliced**: `Ctx` (its context type),
`api` (its `server.Api`, which says how its own error type renders), the `who` function each
route names, and the four names below from `codec` and `server` -- `json_decoder`,
`json_encoder`, `json_list_encoder`, `uuid_param`. They are emitted bare rather than qualified
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
