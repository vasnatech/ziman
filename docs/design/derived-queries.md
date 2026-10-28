# Derived queries: a repository function's name is its query

Status: **built, 2026-10-28.** Everything below is implemented as written; §10 records the
handful of decisions the design left to the implementation and what each one chose. Written first
because the name grammar is the part that has to be right before any of it is worth writing, and
because the corpus that decides the grammar already exists and can be counted rather than guessed
at -- which is also why nothing in §§1-8 needed revising once it was built.

## 1. What exists today

`#![repository(Entity)]` generates nine functions whose bodies the entity decides — `insert`,
`update`, `find`, `find_or_error`, `exists`, `delete`, `find_all`, `count_all`, `delete_all` — and a
name the module defines itself wins over the generated one (see DESIGN.md, 2026-10-26).

That covers the keyed CRUD and nothing else. Every query with a predicate beyond the key is written
out: a `repository.query("SELECT v FROM VitalSign v WHERE ..")` val and a body that binds to it.
After converting all 26 repositories of the sample backend, those bodies are **91 of 144 repository
functions**, and they are mechanical.

## 2. The evidence, counted

The sample backend's repositories are hand-ports of Spring Data interfaces; their own comments cite
fourteen of the originals (`findByPatientIdAndStatusNotOrderByCreatedAtDesc`,
`countByPatientIdAndStatusIn`, `existsByPatientIdAndTypeAndReferenceId`, `findAllByExpiresAtBetween`
and so on). So the names are *already* derived-query names, written by hand against bodies written
by hand.

Of 144 repository function instances:

| | |
|---|---|
| derivable from entity field names alone | **91** |
| derivable with keyword support (`between`, `order_by`, `not`, `ignore_case`) | 4 |
| not derivable — domain words, projections, paging, and every write | 49 |

And the derivable ones are the *repeated* ones, which is what makes this worth building:

| name | instances |
|---|---|
| `delete_by_patient_id` | 19 |
| `find_all_by_patient_id` | 16 |
| `delete` (scoped, `id` + owner) | 15 |
| `find_by_id_and_patient_id` | 12 |
| `count_by_patient_id` | 4 |
| `delete_by_user_id` | 3 |

Six shapes, 69 functions.

## 3. The grammar

```
name       := verb [ "_by_" predicate ] [ "_order_by_" ordering ]
verb       := "find" | "count" | "exists" | "delete"
predicate  := term { "_and_" term }
term       := column [ "_" op ]
op         := "between" | "in" | "not" | "is_null" | "is_not_null" | "ignore_case"
ordering   := column [ "_" dir ] { "_and_" column [ "_" dir ] }
dir        := "asc" | "desc"
```

**There is no `_all`.** Cardinality comes from the return type and nothing else, which is how
Spring Data reads it too -- `findBy` and `findAllBy` are synonyms there. So `find_by_patient_id`
answers one row or many depending on whether it returns `VitalSign` or `[VitalSign]`, and §5 is the
only place that distinction lives. The corpus spells 16 of these `find_all_by_patient_id` today;
those names shorten.

**No `_by_` means no predicate.** `find(conn) -> Result<[E], DbError>` is every row; there is no
form where the predicate
comes from the parameter list. This is the one place the grammar costs churn: the 15 bare
`delete(conn, id, patient_id)` functions become `delete_by_id_and_patient_id`, which is the point —
a scoped delete should say so in its name, which is exactly the argument the `#[scope]` sketch was
trying to answer with an attribute.

## 4. Matching, which is where this beats Spring Data

### 4.1 Columns: longest match against the entity's own list

The tokeniser's dictionary is `entity.columns`. At each position it takes the **longest column name
that is a prefix of the remaining text**, so `find_by_patient_id_and_type` reads `patient_id` and
then `type` rather than `patient` and `id_and_type`.

Two parses is a refusal, not a preference. Checked against the real schema: **no column in the
sample backend's 36 entities contains `_and_`, `_in`, `_not`, `_between`, `_asc` or `_desc`**, so
the grammar is unambiguous there today — but a column named `name_and_type` would make
`find_by_name_and_type` genuinely two-ways readable, and guessing is how Spring Data's resolution
surprises people.

### 4.2 Parameters: by name, not by position

**This is the whole reason to build it rather than copy it.** Spring Data binds parameters
positionally against the predicate terms, so `findByIdAndPatientId(patientId, id)` — two `UUID`s in
the wrong order — compiles, boots, and answers the wrong rows forever. It is the same failure mode
as a transposed `INSERT` column list, which is what `insert_one` and `#[pk]` exist to remove.

So: each predicate term names a column, and the function's parameters after `conn` **must be named
for those columns, in that order**. A mismatch is a compile error naming both.

```ziman
#[repository]
pub fn find_by_id_and_patient_id(conn: datasource.Connection, id: Uuid, patient_id: Uuid)
    -> Result<VitalSign, DbError>          // ok

#[repository]
pub fn find_by_id_and_patient_id(conn: datasource.Connection, patient_id: Uuid, id: Uuid)
    -> Result<VitalSign, DbError>          // refused: parameter 1 is named patient_id, the
                                           // predicate's first term is id
```

The parameter's *type* is checked against the entity's field type for that column too, which catches
the other half: `find_by_patient_id(conn, patient_id: string)` on a `Uuid` column.

Operators change the arity of a term:

| op | parameters |
|---|---|
| none | one, named for the column |
| `in` | one, named for the column, of type `[T]` |
| `between` | two, named `<column>_from` and `<column>_to` |
| `is_null`, `is_not_null` | none |
| `not`, `ignore_case` | as the base term |

## 5. The return type selects the runtime function

| verb | return | becomes |
|---|---|---|
| `find` | `Result<E, DbError>` | `eql.one`, `NotFound` when absent |
| `find` | `Result<Option<E>, DbError>` | `eql.one`, absence is the answer |
| `find` | `Result<[E], DbError>` | `eql.list` |
| `count` | `Result<int, DbError>` | `eql.count` |
| `exists` | `Result<bool, DbError>` | `eql.exists` over the predicate |
| `delete` | `Result<unit, DbError>` | one row, `NotFound` when none matched |
| `delete` | `Result<int, DbError>` | every matching row, answering how many |

That last pair is worth noting: the **return type**, not the name, distinguishes "delete the one row
this addresses" from "delete every row matching". Today that distinction is carried inconsistently by
the name — `delete` versus `delete_by_patient_id` — and by which runtime function the body happens to
call (`deleted_one` versus `delete_where`). Putting it in the type makes it declared.

A verb and return type that do not pair is a refusal.

## 6. Refusals, and why they are compile errors

**Every refusal below is checked in the generator, from the entity's `StructDecl` and the function's
signature, before any SQL exists.** That is a commitment about *where* the check happens, not just a
wish: the generator already receives the `StructDecl` (it reads `#[pk]` from it), so every field
name and type is in hand at compile time, and a `ziman_error` at the function's own `fn->line` /
`fn->col` reads like any other type error:

```
ziman: type error at 31:1 in repository/vital_sign.zm (in derived query 'find_by_patinet_id'):
  entity VitalSign has no field 'patinet_id' (it has: id, patient_id, type, vital_value1, ..)
   |
31 | pub fn find_by_patinet_id(conn: datasource.Connection, patinet_id: Uuid) -> Result<[VitalSign], DbError>
   | ^
```

**This is the difference between a design that works and one that is merely clever.** A derived
query whose name is wrong must fail where the author wrote it. The alternative -- letting the
generated `repository.query(..)` fail when `eql.translate` runs at module initialisation -- was
measured on a real mistake during the adoption: the error arrived with three frames of *library*
code and not one line of the repository that caused it.

```
at repository::translated (repository.zm:150:12)
at repository::query (repository.zm:41:30)
at <module initialisation> (0:0)
```

The one case that stays a load-time error is SQL the generator itself emits badly, which is a
generator bug rather than an author mistake -- the author's input is the name and the signature, and
both are fully checked before any SQL is built.

The refusals:

* a term naming no column of the entity — listing the columns, as `eql` already does for a field
* two readings of one name — showing both
* a parameter whose name is not its term's column, or whose type is not that column's type
* a `between` term without its `_from`/`_to` pair
* a verb and return type that do not pair
* `#[repository]` on a function in a module with no `#![repository(..)]`

## 7. What it deliberately does not cover

The 49 non-derivable instances are not a gap to close later. They are:

* **Every write but the keyed ones** — the 13 partial updates, `patient.lock`,
  `notification.mark_all_read`, the two `RETURNING` inserts, `audit_event.save`. DESIGN.md's
  2026-10-27 entry records why an entity-shaped update can only express a full-row write.
* **Domain words**: `find_current_by_patient_id`, `find_recent_by_patient_id`,
  `find_latest_by_patient_id_and_type`, `find_all_expiring_between`. `current` and `recent` are facts
  about the application, not about the table, and the first is a `CASE WHEN status = 'ACTIVE'`
  ordering that no name should try to carry.
* **Projections**: `patient.find_all_ids` answers `[Uuid]`, not entities.
* **Paging**: `find_page_by_patient_id` needs `LIMIT`/`OFFSET` parameters. Spring spells this
  `Pageable` or `findTop10By..`; neither is decided here.
* **Non-column groupings**: `find_by_entity` (an `entity_type`/`entity_id` pair),
  `exists_for_reference`, `delete_dismissals_by_patient_id` (which names the *other* entity in a
  two-entity module).

All of them stay hand-written under the existing rule, and a hand-written body still wins.

## 8. Decisions

All four questions this document opened are settled. Recorded here rather than folded into the
sections above, so that a reader can see what was chosen *and* what was given up.

1. **No `_all`. Cardinality comes from the return type.** `find_by_patient_id` answers one row or
   many depending on whether it returns `VitalSign` or `[VitalSign]`.

   What this gives up: a call site reading `repository.find_by_patient_id(conn, id)` cannot tell
   which it gets without looking at the declaration. The rejected alternative was to *require* the
   name and the return type to agree -- `find_by_..` one, `find_all_by_..` many, a mismatch being a
   refusal -- which would have made the redundancy checked rather than merely present. Shorter names
   won; 16 names in the corpus lose six characters.

2. **No cap on term count.** The longest in the corpus is three terms
   (`find_all_by_patient_id_and_name_ignore_case_and_id_not`,
   `find_by_id_and_medication_id_and_patient_id`).

   A cap would turn a style problem into a build error while offering nothing better: an author who
   finds a five-term name unbearable will write the query by hand anyway, and a hand-written body
   already wins. The guidance -- not the rule -- is that `repository.query` is always available and
   usually clearer past about three terms.

3. **One generated `Translated` val per derived function**, private, named for the function, with a
   refusal if the name collides with a hand-written val.

   The considered alternative was a single `Map<string, Translated>` for the whole module. It was
   **not** rejected on speed: hashing a 25-byte key is tens of nanoseconds against a round trip this
   layer measures at 300-3,000 microseconds, four orders of magnitude apart, so performance does not
   decide it. It was rejected because (a) the vals are *generated*, so one-versus-N is invisible to
   the author and only changes `--emit-derived` output, (b) a map literal puts every query inside
   one expression, so a load-time failure's position is the map rather than the function, and (c) the
   lookup needs an `unwrap` on a `None` the generator guarantees cannot happen, in generated code
   that `--emit-derived` exists to make readable.

   If a name-indexed registry of a module's queries ever has a consumer, the per-function vals can
   be collected into one without changing a single author-written line.

4. **The marker is `#[repository]` on a bodyless `pub fn`, not a `derived` keyword.**

   A bare `derived` never answers "derived from *what*?", and it would be the first bodyless form in
   the language that does not say what fills it -- `native fn` names the runtime. `#[repository]`
   names the mechanism and reads against the module attribute as the inner/outer split already
   works: `#![repository(VitalSign)]` says *this module is a repository over VitalSign*,
   `#[repository]` says *this function is one of its queries*.

   It is general by construction without needing a registry. The rule is not "there is a generic
   `derived` hole that some generator fills" -- which would need an owner, a what-if-nobody rule and
   a what-if-two rule -- but "a bodyless `fn` carrying attribute X is filled by X's generator". One
   consumer exists today; a second costs no new language surface. That is the mechanism
   `docs/ROADMAP.md`'s code-generation entry asked for, stated as narrowly as it can be.

   Seven declaration attributes already carry behaviour (`effectful`, `compiler_resolved`,
   `when_unmatched`, `deprecated`, `thread_bound`, `at_least_one`, `thread_key`), so this needs no
   new grammar -- where a keyword would.

## 9. What was left before building, and what it became

Nothing in the design. The open work was implementation, in this order, and all three are done:

1. The name parser and its refusals, against a `StructDecl` -- all of §6, checkable without running
   anything. **Done**, `src/derive.c`: `split_verb`, `enum_terms`/`enum_orders`, `refuse_unreadable`.
2. Parameter matching by name and type (§4.2), which is the reason to build this rather than copy
   Spring Data. **Done**: `expected_params` and `check_query_params`.
3. Emission: one query val plus one body per function, spliced at the function's own `fn->line` so
   that anything which does reach a runtime error names the right line. The current repository
   generator splices everything at the *module attribute's* position, which is worse than the val
   and should be fixed with this. **Done, and the fix turned out to be structural rather than a
   changed argument**: a derived query is the author's own `FunctionDecl`, so there is nothing to
   splice *at* -- `parse_and_splice_into` fills in its `body` and the declaration keeps its own
   position, name, parameters and return type. The keyed CRUD still uses the module attribute's
   position, and that is now right rather than lazy: those nine functions appear nowhere in the
   source, so the attribute is the only line they can honestly point at.

## 10. What the implementation decided

Nine questions the design did not reach, each settled while building and each cheap to revisit.

1. **The query val is `<function>_query`, private, built from an inline one-entity registry**:
   `val find_by_owner_query: eql.Translated = unwrap(eql.translate(eql.registry([note_meta]), ".."))`.
   A module-level registry val shared by the file's queries was the alternative; inlining keeps
   §8.3's promise of exactly *one* generated name per derived function, and a single-entity
   `Registry` is a one-entry map built once when the module loads.

2. **A hand-written val of that name is a refusal**, reported at the val rather than at the
   function -- §8.3 asked for it and this is where it landed.

3. **Those vals are appended to `module->vals`, where every other derived val is prepended.** The
   prepend exists so a hand-written val may read a derived one; this is the same ordering fact read
   backwards. A query val calls `eql.translate(.., <entity>_meta)`, and when the entity is declared
   in the same module that `_meta` is *itself* a prepended derived val -- so prepending here would
   put the query ahead of the metamodel it reads and fail at load. Nothing hand-written can name a
   query val, so last is safe.

4. **`ignore_case` is `upper(e.col) = upper(:col)`, not a collation.** A collation is per column and
   per engine; this has to mean the same thing on memdb, SQLite, Postgres and MySQL.

5. **An `Option<T>` column's parameter is `T`.** `col = NULL` matches no row in any engine, so a
   parameter of `Option<T>` binding None would be a query that silently answers nothing. The None
   side of a nullable column is what `_is_null` is for, and it takes no parameter at all.

6. **`exists` is `eql.count(..) > 0`.** `eql.exists` takes an entity and a *key*; there is no
   predicate form of it, and adding one would duplicate `count`'s translation for no round trip
   saved.

7. **An ordering on anything but `find` is refused.** `count_by_owner_order_by_id` sorts rows it
   then does not answer; `DELETE .. ORDER BY` is not portable SQL either.

8. **The generated EQL aliases the entity `e`** (`SELECT e FROM VitalSign e WHERE e.id = :id`). The
   alias never reaches the author, and a one-letter name keeps the `--emit-derived` output on one
   line for the common two-term query.

9. **The bodyless form is a third kind in `parse_signature_only_function`**, beside `native fn` and
   `builtin fn`, rather than a parse of its own -- all three are a signature and no body, differing
   only in who fills it. What that cost elsewhere is one line: `fn_has_body` now asks whether the
   body is there instead of listing the kinds that never have one, because a derived query is the
   first kind that *acquires* one, and no flag can answer the question at both times.
