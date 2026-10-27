# Derived queries: a repository function's name is its query

Status: **design only.** Nothing below is built. Written because the name grammar is the part that
has to be right before any of it is worth writing, and because the corpus that decides the grammar
already exists and can be counted rather than guessed at.

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
name       := verb [ "_all" ] [ "_by_" predicate ] [ "_order_by_" ordering ]
verb       := "find" | "count" | "exists" | "delete"
predicate  := term { "_and_" term }
term       := column [ "_" op ]
op         := "between" | "in" | "not" | "is_null" | "is_not_null" | "ignore_case"
ordering   := column [ "_" dir ] { "_and_" column [ "_" dir ] }
dir        := "asc" | "desc"
```

`_all` is a *cardinality* marker, not a predicate: `find_by_..` answers one row, `find_all_by_..`
answers many. It is spelled on `find` only, because `count`, `exists` and `delete` take their
cardinality from the return type (§5).

**No `_by_` means no predicate.** `find_all(conn)` is every row; there is no form where the predicate
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
| `find_all` | `Result<[E], DbError>` | `eql.list` |
| `count` | `Result<int, DbError>` | `eql.count` |
| `exists` | `Result<bool, DbError>` | `eql.exists` over the predicate |
| `delete` | `Result<unit, DbError>` | one row, `NotFound` when none matched |
| `delete` | `Result<int, DbError>` | every matching row, answering how many |

That last pair is worth noting: the **return type**, not the name, distinguishes "delete the one row
this addresses" from "delete every row matching". Today that distinction is carried inconsistently by
the name — `delete` versus `delete_by_patient_id` — and by which runtime function the body happens to
call (`deleted_one` versus `delete_where`). Putting it in the type makes it declared.

A verb and return type that do not pair is a refusal.

## 6. Refusals

Every one of these is a compile error, not a runtime surprise:

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

## 8. What is still open

1. **Name length.** `find_all_by_patient_id_and_name_ignore_case_and_id_not` is in `medication.zm`
   today. Deriving bodies makes names like that load-bearing rather than merely descriptive, which is
   the standing criticism of Spring Data's scheme. No answer proposed; worth deciding whether the
   grammar should cap term count and push the rest to a hand-written query.
2. **Where the generated query val goes.** Each derived function needs a `Translated` built once at
   load. Emitting one val per function is the obvious shape and makes the module's `--emit-derived`
   output large.
3. **Resolved: an attribute naming its generator, not a `derived` keyword.** The marker on a
   bodyless declaration is `#[repository]`, not `derived`.

   A bare `derived` leaves "derived from *what*?" unanswered at every site, and it would be the
   first bodyless form in the language that does not say what fills it -- `native fn` names the
   runtime. `#[repository]` names the mechanism, and reads against the module attribute as the
   inner/outer split already works elsewhere: `#![repository(VitalSign)]` says *this module is a
   repository over VitalSign*, `#[repository]` says *this function is one of its queries*.

   It is also **general by construction without needing a registry.** The rule is not "there is a
   generic `derived` hole that some generator fills" -- which would need an owner, a what-if-nobody
   rule and a what-if-two rule -- but "a bodyless `fn` carrying attribute X is filled by X's
   generator". One consumer exists today; a second costs no new language surface. That is the
   mechanism `docs/ROADMAP.md`'s code-generation entry asked for, stated as narrowly as it can be.

   Seven declaration attributes already carry behaviour (`effectful`, `compiler_resolved`,
   `when_unmatched`, `deprecated`, `thread_bound`, `at_least_one`, `thread_key`), so this needs no
   new grammar at all -- where a keyword would.
4. **Whether `_all` should exist at all**, given the return type already says `[E]`. It is redundant
   with `Result<[E], DbError>`, and dropping it would shorten every list name — at the cost of
   `find_by_patient_id` meaning one row or many depending on a type a reader has to look at.
