# `derive_repository`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#![derive(Repository(Entity))]`, written in Ziman: the CRUD every repository writes the same way.

**The first generator of the second kind** (docs/design/meta.md §8, decided 2026-10-29). The
five derives take a `meta.Decl` and are called once per declaration; this one takes a
`meta.Target` -- an entity declared in *another file*, plus what the current module already
defines -- and is called once per module. That is the whole reason the kind exists: none of
what it reads is a function of one declaration.

The functions below are the ones whose body is decided entirely by the entity: its table, its
column list and the `#[pk]` fields that address one of its rows. A repository's *domain*
queries are not here -- "this patient's vital signs, newest first, page 3" is a fact about the
application, not about the entity. A query whose name says what it asks is a derived query and
is generated separately; one that does not is written out by hand.

**A name the module already defines wins, and nothing is emitted for it.** That rule is what
makes the attribute safe to add to a repository that has already been written: the North Star's
`repository/vital_sign.zm` scopes its `update` and `delete` by the patient as well as the id --
the defence that makes another patient's row "not found" rather than moved -- and a generated
keyed version replacing it would silently remove a guarantee.

`use derive_repository` -- or `use derive_repository.Name` to bring one item in bare.

### `generate_module`

```
pub fn generate_module(t: meta.Target) -> Result<string, string>
```

## The SQL, and the body that runs it

### `generate_query`

```
pub fn generate_query(t: meta.Target, f: meta.Function) -> Result<string, string>
```
