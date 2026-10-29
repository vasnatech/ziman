# `derive_entity`

> **Experimental** -- see docs/DESIGN.md's "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

`#[derive(Entity)]`, written in Ziman: the column list an entity's queries name, and -- when
the attribute carries a table -- the table name and the `eql.Entity` metamodel beside it.

**The last of the five, and the first to read `Decl.arg` and a field attribute.** Everything
it needs that the other four did not is what steps 3 of docs/design/meta.md added: the string
in `#[derive(Entity("medications"))]`, and `#[pk]` as an entry in a field's `attrs`.

What it emits only what the compiler already knows for certain. In particular it does **not**
guess the table name from the type: of the North Star's eighteen entities, three break every
pluralisation rule (`FamilyHistory` is stored in `family_history`, `MedicalHistory` in
`medical_histories`, `UserNotificationPreference` in `user_notification_prefs`), and a wrong
guess generates SQL against a table that does not exist.

The entity *name* is the struct's, not the table's: `eql` matches what a query writes
(`FROM Patient p`) against it, and taking it from the table would make `FROM patients p` the
spelling -- SQL leaking back through the layer whose job is to hide it.

`use derive_entity` -- or `use derive_entity.Name` to bring one item in bare.

### `generate`

```
pub fn generate(d: meta.Decl) -> Result<string, string>
```
