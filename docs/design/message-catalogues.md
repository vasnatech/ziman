# Message catalogues

Status: **proposed, 2026-11-20.** Nothing below is written. This is **P2** of
[the health-record port](health-record-port.md), answered "design now, build later" on
2026-11-20: the shape is settled here so that every screen is built against it from the first one,
and the 1,709 keys are loaded whenever that is wanted. Retrofitting a catalogue across finished
screens is the expensive half, and it is the half this document exists to avoid.

## 1. What is actually there

Measured, not assumed, from `health-record-frontend-node/src/locales`:

| | |
|---|---|
| Keys | **1,709**, in four files (`en`, `es`, `fr`, `tr`) |
| Nesting | up to **4** levels; flattens to dotted names |
| Interpolation | **100 keys, 119 placeholders**, all named: `Step {{current}} of {{total}}` |
| Plural forms | **none** |
| Markup in a value | 1 key |

**There are no plurals, and that is the single most useful fact here.** `count` appears in fifteen
`t()` calls and every one of them passes it as a *value*, not as a selector: `{{count}} days`,
`{{count}} servings`, `{{count}} selected`. So the expensive, locale-specific part of every
catalogue library -- Polish's four forms, Arabic's six, and the rule tables that pick between them
-- **is not needed to port this application**, and building it first would be building the hardest
thing for nobody.

It also means the original prints "1 days". That is a defect to leave behind rather than port, and
it is the reason §5 keeps a door open rather than nailing it shut.

## 2. What the target's own test already knows

`src/locales/locales.test.ts` is worth reading before designing anything, because it is a record of
what went wrong rather than of what was feared:

- **The four files drifted.** `fr.json` carried 30 keys under a prefix no `t()` call could reach.
- **Drift has a direction.** A key in `en` and missing elsewhere **renders as the raw key id on
  screen** in that language. A key present only in `fr` is invisible dead weight that reads as
  translation coverage.
- **An empty string renders as a blank label**, which looks like a bug in the layout rather than a
  hole in the catalogue.
- It compares key *sets* and never values, deliberately: a key left in English everywhere is a
  translation question and not a structural one.

**And the dead keys survived because the key was computed**: `t(\`dashboard.encounters.type_${type}\`)`.
No static check can see that key, which is why thirty wrong ones sat there. This is the hole, and
§4 is about it.

## 3. The shape

A catalogue is **a locale, a table of dotted keys, and a fallback**. In `lib/core/`, not
`lib/ui/`: a notification body and a CLI message want the same table, and a module that lived under
`ui` would be reached for by a server through the wrong door.

```ziman
pub struct Catalogue {
    locale: Locale
    // Dotted keys: `dashboard.medications.daysSupplyValue`.
    entries: Map<string, string>
    // What answers when this one cannot. English, in practice.
    fallback: Option<Catalogue>
}

pub fn text(c: Catalogue, key: string) -> string
pub fn text(c: Catalogue, key: string, args: Map<string, string>) -> string
```

Four decisions, each with its reason:

1. **Dotted keys, flattened at load, not a nested lookup.** The files are nested and the lookups
   are not; flattening once at load makes every lookup one map read, and makes a key a value that
   can be held, passed and listed. It is also what lets §4's gate compare two catalogues at all.
2. **Named interpolation only** (`{{current}}`), never positional. All 119 placeholders are named
   already, and a positional form would let a translator reorder a sentence and silently swap two
   values -- which is the whole reason named placeholders exist.
3. **A missing key answers with the key**, exactly as the target does today, and **never with "".**
   A raw key on screen is ugly and findable; a blank label looks like a layout bug and gets
   investigated in the wrong file for an afternoon.
4. **No plural machinery.** See §1. A `Catalogue` that cannot pluralise is honest about what it
   does; one that pretends to, with a single English rule behind it, would be worse than both.

## 4. The gate, which is the point of designing this early

Three checks, all cheap, all over the files rather than over the code:

1. **Every catalogue has the same key set as the reference one.** Both directions, because they
   fail differently: a key missing from `tr` shows on screen, a key only in `tr` is dead weight
   that looks like coverage.
2. **No value is empty.**
3. **Every placeholder in a translation appears in the reference string for the same key.** The
   target's test does not check this and it is the next drift waiting to happen: a translator who
   writes `{{total}}` where the English says `{{count}}` produces a string that renders the
   placeholder literally, in one language, on one screen.

**The dynamic key stays a hole and should be made a small one.** A key built at runtime cannot be
checked by any of the above. The answer is not to ban it -- `type_${enum}` is a reasonable thing to
write -- but to make the *family* declarable, so a gate can check that every member of an enum has
a key and that no key under that prefix is unreachable. That is what would have caught the thirty
dead `symptom_*` keys the day they were written.

## 5. What is deliberately not here

- **Plural rules.** §1. The door stays open: a later `plural(c, key, n)` can be added without
  changing anything above, because `text` does not know about numbers at all.
- **Gender and case.** Turkish is among the four languages and agglutinates; none of the 1,709
  strings depends on it today.
- **Loading from disk.** `json` reads the files and `fs` finds them; neither needs designing.
- **A translation workflow.** Out of scope, and the catalogues already exist.
