# `endpoint`

What every JSON endpoint is built from: the media type, the decoders and encoders over a model's
own `#[derive(Json)]` functions, and the readers that turn a captured path segment into a `Uuid`.

**Why these are here and not in each application.** They were a project's own file until
2026-10-31 -- eight declarations, and not one of them mentioned anything of that project's: a
decoder over `from_json` is the same decoder in every application that derives JSON, and
`uuid_param` is the same reader. Keeping them per-project meant every application rewrote them
and `#![derive(Routes)]` (lib/http/derive_routes.zm) could not name them.

**What deliberately stayed behind.** An application's `Api` -- how *its* error type renders --
and the `who` functions an endpoint names, because both are about types this module cannot see.
`#![derive(Routes)]` emits those as bare names, resolved where the generated code is spliced.

`use endpoint` -- or `use endpoint.Name` to bring one item in bare.

### `application_json`

```
pub val application_json = mime.application_json()
```

JSON, which is the one media type these helpers are about. An endpoint answering something
else builds its own `server.Encoder` and does not come through here.

### `json_decoder`

```
pub fn json_decoder<I>(from_json: fn(Json) -> Result<I, string>) -> server.Decoder<I>
```

A decoder for JSON bodies of any model: the bytes are parsed, and `from_json` -- the model's own derived decoder (`#[derive(Json)]`'s
`<type>_from_json`) -- reads the tree. A body that is not JSON is a 400 `malformed JSON: ...`, one that does not fit the model a 400 naming the field.

### `json_encoder`

```
pub fn json_encoder<O>(to_json: fn(O) -> Json) -> server.Encoder<O>
```

The other direction, and the counterpart of `json_decoder`. `to_json` is the model's own derived
encoder, `#[derive(Json)]`'s `<type>_to_json`.

### `json_value_encoder`

```
pub fn json_value_encoder() -> server.Encoder<Json>
```

A `Json` value as the whole body, for an endpoint answering a tree it passes through rather than a
model of its own.

### `count_encoder`

```
pub fn count_encoder() -> server.Encoder<int>
```

### `list_encoder`

```
pub fn list_encoder<O>(to_json: fn(O) -> Json) -> server.Encoder<[O]>
```

### `uuid_param`

```
pub fn uuid_param(kind: string) -> fn(server.Params) -> Result<Uuid, string>
```

A `{id}` path parameter is just a string; this turns it into a `Uuid`, and a bad one is answered 400
before the endpoint runs. `kind` names what the id belongs to, for the message ("invalid order id").

### `named_uuid_param`

```
pub fn named_uuid_param(name: string, kind: string) -> fn(server.Params) -> Result<Uuid, string>
```

The same as `uuid_param`, but for a captured segment named something other than `{id}`: a pattern with
two captures (`{orderId}/items/{itemId}`) needs to say which it means.
