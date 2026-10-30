# Design journal, part 7 (entries dated 2026-10-23 to 2026-10-25)

Part of the [design journal](README.md) -- see its "How to read this journal" for what a dated entry means.

## Text counts characters, and the ASCII bit that made it affordable (2026-10-23)

`len("héllo")` answered 6. The text is five characters; 6 is the number of bytes UTF-8 needs to store it. Every index function agreed with that answer -- `substring`, `char_at`, `index_of`, `last_index_of`, `is_char_at` all took and answered byte offsets -- so the library was consistent, documented, and wrong in the one direction that no test finds: bytes and characters are the same number for ASCII, and almost every fixture is ASCII.

This moved the whole family to characters. `len` first, with `byte_len` taking over the byte question and `char_count` deleted; then the five read functions, because `len` in characters beside `substring` in bytes left the core parsing idiom -- `substring(s, 0, index_of(s, ",", 0))`, 102 call sites -- pairing one unit against the other.

**The type system cannot find these call sites, and neither can the suite.** A byte offset and a character index are both `int`, so nothing fails to compile, and ASCII fixtures agree with both readings. What found them was deleting the two declarations from a throwaway interpreter and letting the compiler name every call that passes text to `len`: 359 errors, then 98, 22, 3, 0 as they were fixed, and 78 → 9 → 5 → 0 in the sample backend. 574 call sites across two repositories moved to `byte_len` with their behaviour unchanged.

**Then the cost arrived, exactly where the 2026-09-29 entry said it would.** Storing UTF-8 and indexing by character means `char_at(s, i)` walks from the start of the string. A scanner pays that per position, and `while i < len(s)` rescans the whole string per iteration, so an ordinary scanner became quadratic twice over -- and the library is ~86 such loops across 21 files (markdown 21, toml 14, yaml 10, xml 6). Measured on a scan of ASCII text: 2,000 characters 4 ms, 4,000 21 ms, 8,000 55 ms, 16,000 128 ms. `tests/lang/yaml_flow_nesting_cap` stopped finishing at all on its 100,000-character case.

**The fix is one bit in the string Value.** `Value.as.str` is `{const char *ptr; uint32_t len; uint32_t flags;}` -- 16 bytes with no padding, which is the size of `Value`'s whole union, and `flags` was a `uint32_t` carrying a single "is NUL-terminated" bit. It is now a bit set: `ZSTR_TERM` as before, plus `ZSTR_ASCII` for "no byte here has its high bit set", with 30 bits still spare. When `ZSTR_ASCII` is set a character index *is* a byte index, so each of the six functions is two implementations and the fast one is the byte arithmetic it was before any of this. ASCII scans went back to linear: 16,000 characters is 2 ms rather than 128, and the yaml case finishes in 0.34 s.

Three facts made that cheap rather than invasive. Only **three** places in `src/` build a string Value -- `value_string_view` (the funnel), `crossing_string_copy`, `gc_copy_string` -- so a once-per-string computation has three insertion points, and the two copies inherit the bit because they copy byte for byte. `string_slice` is the only view producer holding its parent, and **a slice of all-ASCII text is all-ASCII**, so it inherits with no scan -- which is what keeps `chars`, `split` and `lines` linear instead of rescanning once per piece. And the whole byte-level machinery underneath is untouched: `value_cstr` and its 77 call sites, `memcmp` equality and ordering, FNV hashing over bytes, zero-copy slices, zero-copy literals, every socket and file read.

**Python's PEP 393 was the alternative, and it was rejected for a structural reason.** Storing 1, 2 or 4 bytes per character -- the narrowest that fits -- gives O(1) indexing for *all* text rather than only ASCII, and the width would even fit in the spare bits with views still working. But Python can afford it because a `str` is a heap object with room for a lazily cached UTF-8 copy, which is what `PyUnicode_AsUTF8` hands to C. A Ziman string is a 16-byte borrowed view with no header and nowhere to cache, so `value_cstr` -- every file path, env var, SQL string and `exec` argument -- would transcode per call, forever. Width-1 is Latin-1 and not valid UTF-8, so European text would transcode on output too. Equality, `memcmp` ordering (correct today only because UTF-8's byte order is code-point order) and byte-wise hashing would each need width-aware rewrites, with wrong answers that ASCII fixtures cannot catch. Trading a known O(n) on non-ASCII indexing for an unknown cost at the C boundary plus a silent-wrongness class was the wrong direction.

**What it still costs, stated rather than hidden**: non-ASCII text walks, so random access into it is O(n) and a scanner over it is quadratic -- measured at 345 ms where ASCII is 2 ms. Performance has two regimes, and one accented character can change a benchmark by thirty-fold. The known remedy is a per-thread one-entry cursor memo, which would make forward scans O(1) amortised and needs no space in a `Value` at all; it is deferred until a benchmark asks for it rather than built blind.

**The sweep found about thirty real bugs, all of the same shape**: a `byte_len` used as the bound of a character-indexed scan. Because a byte count is never smaller than a character count, each fails only on multi-byte input. `mime.parse` handed a byte length to a character `substring` and raised; `toml`'s date sniff cleared a 10-*byte* guard and then read character 7 of a 6-character string; `properties`' "did you mean" edit distance sized its first row in bytes and read its last cell by byte index, running off the end of every later row; `zoneoffset_from_string` accepted a 6-byte, 3-character offset; `uuid`'s error counted bytes while its own words said "36 characters"; `xml`'s BOM strip removed three characters where a BOM is one. `tests/lang/library_scans_count_characters.zm` runs non-ASCII text through each.

Regex needed its own decision. The engine matches bytes and reports byte offsets, so the conversion happens in `caps_to_char_indices` at the one point offsets leave C -- which left `regex.zm` needing no byte-slicing of its own, and makes `Match.start`/`end` character indices that compose with `substring` and `len` like any other. Outside `regex.zm` those fields were read in exactly two test printfs, so three expected numbers changed and nothing else did.

A `MutableString`'s four index functions moved with their immutable twins, because `len(sb)` had already become a character count and `char_at(sb, 1) == char_at(s, 1)` could be false for the same text. A builder has no `Value` to carry `ZSTR_ASCII` in, so `MutableStringBuf` keeps the flag itself, maintained by its five writers and reset by `clear`. `set` went from replacing one byte to replacing one character, which means it now *splices*: `e` over `é` shortens the buffer, `é` over `e` lengthens it. The character count -- what `len` answers -- is what `set` leaves alone, so "one element in, one element out" holds in the unit the function speaks; refusing unequal byte lengths instead would have left `set` working on ASCII and failing on everything else.

**`reverse` on text was quadratic for an unrelated reason, and the bit fixed it too.** It was ordinary Ziman: `chars(s)` into an array, then folded with `concat` in a loop. That fold is the quadratic string build -- every `concat` allocates a fresh buffer and copies everything accumulated so far -- so it copied ~n²/2 bytes, and paid n+1 allocations for the `chars` array and its views before starting. Measured: 4,000 characters 10 ms, 8,000 22 ms, 16,000 54 ms. It is a native now, one allocation of exactly the right size and one pass, and reversing ASCII text does not decode UTF-8 at all because one byte per character means reversing the characters *is* reversing the bytes. 16,000 characters went from 54 ms to about 0.01 ms, and both paths are linear (ASCII 128,000 characters x 200 in 9 ms; non-ASCII in 56 ms). The result inherits `ZSTR_ASCII` exactly rather than conservatively: a reversal moves bytes without changing them, so it is ASCII precisely when its input was. Registering it needed the question `char_at` and `last_index_of` had both answered the wrong way -- the array member is `array_reverse`, not a bare `reverse`, so `string_reverse` is safe to add.

Verified: `make check` green, 1,162 fixtures on both the collected and `nogc` builds, including the yaml case that previously timed out, the 50-task benchmark suite, and the North Star backend's 61 suites / 2,603 checks. `tests/bench/string_scan.zm` asserts the ASCII path stays linear and measures the non-ASCII path separately, so the two regimes cannot quietly merge.


## A locale comes from the OS, and the default deliberately does not (2026-10-23)

`to_upper("ID")` is `"ID"` everywhere except Turkey, where the i has no dot and the upper case of `i` is `İ`. Ziman had no way to say which answer it wanted, because it had no locale at all -- and the immediate need was smaller and more concrete: `MMM`/`MMMM` in a date pattern have to come from somewhere, and a month name is a language's, not a program's.

**The API is POSIX 2008 and nothing older.** `newlocale`/`freelocale` make and own a `locale_t`; `nl_langinfo_l`, `towupper_l`, `towlower_l` and `strcoll_l` each take one explicitly. `setlocale` and `localeconv` are never called, not once: they mutate process-global state, they are not thread-safe, and a library that changes the host program's locale behind its back is a bug in every language that has shipped one. A `Locale` is therefore an ordinary handle -- a `HandleSet` entry with a finalizer that calls `freelocale`, the pattern `native_regex.c` already used -- so a forged handle is caught by membership rather than by a magic word, and a dropped locale is freed by the collector.

**The runtime default is `locale.root()`, not the system locale**, which is the decision in the entry worth disagreeing with. The argument for the system locale is that it is what the user wants; the argument against, which won, is that a program must produce the same output on every machine unless it asks not to, and a default read from the environment means `to_upper` and a formatted date change meaning when a deployment sets `LANG`. `locale.use_system()` is one call, and it is a decision the program makes out loud. Root is the C/POSIX locale: ASCII case mapping, byte collation, English names.

That default is also what keeps the cost at zero. `ziman_locale_default_is_root()` guards the ASCII fast path in `to_upper`, `to_lower` and `equals_ignore_case`, so text that was folded by table lookup before still is, and only a program that has asked for a real locale pays `towupper_l` per character. The same guard is why adding locale support changed no benchmark.

**Locale-aware functions gained an overload with a trailing `Locale`** -- `to_upper(s, l)`, `to_lower(s, l)`, `equals_ignore_case(a, b, l)`, `compare(a, b, l)` -- and the forms without it use the runtime default. `compare` needed the compiler opened rather than just a declaration: it is a builtin, and the builtin arm in `typecheck.c` checks arity before overload resolution ever runs, so a third argument was rejected with "takes exactly 2 arguments" by code that had never heard of the overload set. `Locale` itself lives in the prelude, not in `locale.zm`, because `string.zm`'s declarations name it.

**Only a UTF-8 or ASCII codeset is accepted.** `locale.of("tr_TR.ISO-8859-9")` resolves to root instead, because `nl_langinfo_l` would otherwise hand back month names in Latin-1 and those bytes are not Ziman text. `of` falls back rather than carrying an `Err`, since a program that names a locale the host has not installed should still run; `locale.name_of(l)` is the POSIX name that actually opened, and comparing it against what was asked is how a caller detects the fallback. `open_with_codeset` tries `.UTF-8`, then `.utf8`, then the bare name, so `locale.of("tr_TR")` finds the installed one; a name in BCP-47 shape converts its dashes to underscores *only before the first `.`*, which is what makes `tr-TR.UTF-8` work -- the codeset has a dash of its own, and converting it produced the unfindable `tr_TR.UTF_8` for one build. A 31-language table maps a bare language to its usual territory, so `locale.of("tr")` resolves.

**Date and day names, and what they cost.** `MMM`/`MMMM` render the month and `EEE`/`EEEE` the day, from `nl_langinfo_l`'s `ABMON`/`MON`/`ABDAY`/`DAY`, longest run first so `MMMM` is one token rather than `MM` twice. The price is that **no name token has a fixed rendered width** -- `ja_JP` abbreviates January as `1月` -- so `DateTimeFormat.width` is -1 for any pattern holding one, and `instant_parse_pattern` (`parse_instant` after the next entry) had been using `width` to find where the calendar part of an instant ended and its offset suffix began. It takes that from how far the parse actually got (`ParsedFields.consumed`) instead, which is what made variable-width names affordable at all. Parsing folds case under the same locale, since a reader handed `"12 EKIM 2026"` means Ekim, and a parsed day name is *checked against the date* rather than stepped over, so `Monday, 23 October 2026` is an error rather than a silently ignored word.

Verified the way the insulation claim deserves: with the default forced to `tr_TR.UTF-8`, all 1,170 fixtures ran and **1,169 were unchanged** -- the one that moved being the locale fixture that asserts the default. A library that quietly depended on ASCII case folding would have failed loudly there.


## The time module drops its type prefixes (2026-10-24)

`localdate_add_days(d, 1)` names the type twice: once in the function name, once in the argument it is given. Overload resolution has picked functions by argument type since F167, and `lib/time/time.zm` was the largest module still spelling the type out, so 47 names moved and 833 call sites across both repositories moved with them. `format` is now one overload set over all seven types, including the `DateTimeFormat` and `Locale` forms -- twenty declarations under one name -- and so are `add`, `subtract`, `negate`, `diff`, `add_days`/`_months`/`_years`/`_hours`/`_minutes`/`_seconds`/`_nanos`, `diff_days`, `diff_nanos`, `to_epoch_day` and `is_leap_year`. `to_string`/`from_string` went too, which also merged the ISO pair into the pattern pair's name: the ISO form was always the pattern form with a preset.

**Three families kept a prefix, each for a reason that is not taste.** `<type>_compare` stays because `Ord` looks the function up *by name* -- `ziman_compare_function_name` in `derive.c` builds `localdate_compare` from the type and calls it -- so a bare `compare` would need a different lookup in the compiler, and `compare` is already a builtin besides. `_to_json`/`_from_json`/`_to_sql`/`_from_sql` are found the same way, by `#[derive(Json)]` and `#[derive(Row)]`. And the constructors (`localdate_of`, `localtime_of`, `duration_of_seconds`, `instant_of`, `zoneoffset_of_seconds`, `zoneid_of`) differ only in *return* type, which overload resolution does not see.

The parse family is the same problem one step on: `parse(s: string, fmt: DateTimeFormat)` is a single signature that four types want. So each gets a name that says which -- `parse_localdate`, `parse_time`, `parse_datetime`, `parse_duration`, `parse_offset`, `parse_zone`, `parse_instant` -- and the rule that distinguishes the module's two naming families is now sharp: a name is bare when the arguments say which type, and carries the type when they do not.

**Three traps, all of them instructive.** `is_leap_year` was a `pub` one-line wrapper over a private function of the *identical* signature, so dropping the prefix turned it into a duplicate definition -- the rename found a redundancy the prefix had hidden, and it also made obvious what the old name obscured: the function takes a `year: int`, never a `LocalDate`. Forty-two calls inside `time.zm` were to the string-interpolation builtin `format("... {}", x)`, which the module's new `format` overload set shadowed; they are `concat` now. And two fixtures had *two* old names collapse onto one import -- `localdate_parse_pattern` and `localdate_from_string` both became `parse` -- which the loader reports as "'parse' is imported more than once under the same name", a diagnostic that only exists because imports are by name rather than by signature.

Verified: `make check` green, 1,173 fixtures on both builds, the 50-task benchmark suite, and the North Star backend's 61 suites / 2,603 checks.


## `prelude.` reaches a name you have taken (2026-10-24)

A file that declares `format` has taken the word. `lib/time/time.zm` gained a `format` overload set the day before, and `self_qualify_module` had already rewritten the module's own declarations to `time::format`, so a bare `format("expected a month name at position {}", pos)` in that same file matched `time::format` before any builtin was considered -- and 42 of the module's own error messages had to be rewritten as `concat`, not because `concat` was better but because `format` had become unsayable there. A program shadowing a builtin (F164, 2026-10-18) has the same hole from the other side: the shadow is deliberate and the original is then unreachable.

**`prelude.name(...)` is the escape, and it is the one qualifier with no module behind it.** It names the implicit namespace -- the prelude's declarations and the handful of builtins the interpreter dispatches without one -- which is what "prelude" already means colloquially: the things you get without a `use`.

The resolution is almost free, and the reason is worth recording. A bundled module's declarations are renamed to `<path>::name`, and the loader's `self_qualify_module` **skips a dotted callee** precisely because it might be a module reference -- so stripping `prelude.` afterwards leaves a bare `format` that *cannot* match `time::format`. One qualifier dropped in `resolve_expr_refs` does the whole job for every bundled module. The entry module is the case needing more: its own declarations stay bare, so the name alone cannot tell the program's `len` from the prelude's, and a flag on the call node (`prelude_qualified`) is what makes `check_call` ignore `shadowing_fn` and makes `overload_resolve` drop every candidate whose `source_file` is non-NULL.

**That filter is the part that could have been skipped and should not have been.** Without it `prelude.len(my_struct)` resolved to the program's own `len` -- the overload set is collected by name from one module list, and the program's declaration is in it. The call compiled, ran, and answered 103 where the qualifier was an explicit request not to look there. A qualifier that silently means nothing in some cases is worse than no qualifier, so a name that is not in the implicit namespace is rejected outright, with its own message rather than "call to undefined function 'helper'" -- which drops the `prelude.` the author wrote and sends them looking for the wrong thing.

**A module genuinely imported under the name `prelude` keeps it.** `find_use` is consulted first, so a project with its own `prelude.zm` is unaffected; the escape is not a reserved word, and nothing had to be taken away to add it.

Not extended to `spawn prelude.f(...)`, which would need the same flag on the spawn statement: spawning a prelude function means spawning a native with explicit type arguments, and no use case for it exists. Function *references* (`val f = prelude.len`) are likewise unsupported, which costs nothing -- a reference to any overloaded name is already rejected, builtin or not, and the advice is the same: wrap it in a lambda, which may hold a `prelude.` call.

**`lib/time/time.zm` was converted back the same day**, which was the user's call against my recommendation and the better one. My argument for leaving it was that `err(concat(...))` is 124 sites across `lib/` against `err(format(...))`'s 82, so neither spelling is the house style and the churn would buy nothing. What that misses is that the 42 `concat` calls were never a choice between two acceptable spellings: they are what the module was *forced* into, and leaving them would have left the file reading as though `concat` had been preferred there. Forty-four call sites moved (`str()` dropping out of each, since `{}` displays what `str` accepts), the error text is byte-identical -- the fixtures that assert it pass unchanged -- and four were left as `concat` because they have no template in them at all: `pad_left`'s per-digit append, the pattern scanner's per-character append, and two that join computed strings with no literal between them. A rule fell out of doing it: `concat` is for appending, `format` is for a message with values in it, and a call with no literal text is the first kind.

Found and fixed while documenting this: SYNTAX.md's "Builtins" section still opened with "Builtin names are reserved -- redefining any of them as a user function is a type error", which F164 had made false twenty-three entries earlier and which contradicted the shadowing bullet in the same file.


## The time module's converters drop their prefixes, and derive learns the exception (2026-10-24)

`instant_to_localdatetime(i, offset)` named both types in a call that already had both in hand, and `local_date_to_json(d)` named one of them twice. Two days after `format` and `add_days` went bare, the conversion families followed: 32 names, 104 call sites.

**The two directions are not symmetric, and that asymmetry is the whole design.** `to_json(d)`, `to_sql(d)`, `to_localdatetime(i, offset)` and `to_instant(dt, offset)` are each told apart by their arguments, so all seven types share one name per direction -- `to_json` is seven declarations, `to_localdatetime` is four (a `ZoneOffset`, a `ZoneId`, a `Json` and a `SqlValue`). Their *readers* all take a `Json` or a `SqlValue`, which carries no information about what to build, so each needs the type in its name: `to_localdate`, `to_localtime`, `to_localdatetime`, `to_duration`, `to_zoneoffset`, `to_zoneid`, `to_instant`. It is the same rule that kept `parse_localdate` and `parse_time` apart two days earlier -- a name goes bare exactly when the arguments say which type -- and it is why `to_json`/`to_instant` can be one name while `from_json` could never have been.

**The blocker was real and it was in the compiler, not in the module.** `#[derive(Json)]` does not look a converter up; it *generates a call by name*, `pascal_to_snake(Type) + "_to_json"`, and `derive.c`'s `STANDARD_TYPES` table synthesises the matching `use time.local_date_to_json` so the deriving file needs no imports. Renaming the functions without touching that leaves every derived struct with a `LocalDate` field calling a name that no longer exists -- confirmed by renaming one and watching `#[derive(Json)]` fail on a two-field struct. So `derive.c` gained `standard_flat_type_name`, consulted at the four emit sites and by the import synthesiser, which the comment on each warns must stay in step: get them out of step and a generated call has no import, reported against the deriving declaration.

**What decided the scope was a measurement, not taste.** The obvious generalisation -- let *every* `#[derive(Json)]` type have `to_json` too, and drop the prefix language-wide -- is not merely large, it does not work: `OVL_MAX_CANDIDATES` is 32, the reference backend alone has 100 `#[derive(Json)]` sites, and an oversized set resolves as `OVL_UNSUPPORTED`, which falls back to `find_function`'s *first* declaration rather than reporting anything. One `to_json` per program is structurally unavailable at any plausible cap, and raising the cap would put a hundred-candidate resolution behind every call. So the short names are vocabulary for a fixed, small set of library types, and `<snake_type>_to_json` stays the rule for everything generated. `Uuid` keeps `uuid_to_json`: its name has nothing to shorten, and the exception is worth having exactly one reason.

**The cost, stated rather than discovered later.** A module that declares its own `to_json` binds the name, `ensure_named_import` leaves it alone by design, and a derived field of a time type then resolves to that function: `argument 1 of 'to_json' expects int, found time.LocalDate`, raised in the derive's own error context so it is traceable. `local_date_to_json` could not collide that way. Judged worth it for the hundred call sites that get shorter, and the message names the derive, which is what makes it a diagnosable mistake rather than a mystery.

**A process note worth keeping.** Verifying this, I ran `ziman --fmt --write` over every `.zm` under `tests/`, which rewrote thirteen fixtures whose exact text *is* the fixture -- the formatter's own input/output pairs, and the diagnostic fixtures whose expected messages quote line and column. The suite went from 1,176 passing to 13 failing in one command, and none of the failures had anything to do with the change under test. Format the files a change actually touched, never the tree.


## `bits` stops naming the type, and a BitSet becomes a set (2026-10-24)

`bits.int_and(a, b)` named the type in a call that already had two ints in it, and `bitset_cardinality(bs)` invented a word for a question Ziman already asks as `len`. The module had 48 prefixed names; it now has 8 operations and 6 constructors.

**The scalar half is the ordinary prefix drop.** `and`, `or`, `xor`, `not`, `shift_left`, `shift_right`, `popcount` and `bit_length` are each one overload set over `byte`, `int`, `bigint` and, where it means something, both sets. The arguments say which is meant, which is the same rule `format` and `add_days` moved under two days earlier.

**The set half is the part worth arguing about, and the argument is that a `BitSet` is a set.** It was modelled on `java.util.BitSet` and inherited that class's vocabulary wholesale -- `cardinality`, `length`, `and`, `or`, `and_not`, `toString` -- which meant a reader who already knew Ziman's `Set<int>` had a second API to learn for the same questions. So `len`, `is_empty`, `remove`, `union`, `intersect`, `difference`, `is_subset`, `to_array` and `format` now mean here exactly what they mean for a `Set<int>`, with `union`/`intersect` variadic because the prelude's are. Two gaps closed themselves on the way: `is_subset` and `to_array` had no equivalent at all before, and `to_array` is *sorted*, a bit index having a natural order a hash set's iteration does not. Only `xor` keeps a bitwise name, `Set<T>` having no symmetric-difference function to borrow one from. The bit-indexed accessors keep `set`/`get`/`flip`, which is what was asked for over `add`/`contains`: they are java.util.BitSet's own names, and a bit at an index is the one place the bit framing is the clearer one.

**It needed a compiler change, in the oldest place that still had a type prefix in it.** A `native fn` finds its C implementation by name, and when a declared name has several implementations the interpreter derives a symbol from parameter one's type -- `array_` + `len`. Two things were missing. A *scalar* had no prefix at all, because nothing had ever needed one (`prelude.c`'s own comment says so), so `byte_`/`int_`/`bigint_` are new entries. And the fallback pasted its prefix onto the whole `fn->name`, which for a bundled module is `bits::and` -- giving `byte_bits::and`, a symbol nobody registered. `bits` is the first module in the project to put two native implementations under one declared name, so it is the first to reach that fallback at all; the prefix now goes on the bare name, inside the module's namespace, yielding `bits::byte_and`. A prelude declaration's name has no `::`, so the split is a no-op there and nothing about the existing behaviour moved.

**The cost is real and lands on the caller.** Several of these names are the prelude's, and a per-name import *replaces* the bare name for the whole file rather than adding to the prelude's overload set: after `use bits.len`, `len("abc")` does not type-check. That is not new behaviour -- it is what `use` has always meant -- but short names make it reachable where `bitset_cardinality` never could. It is survivable because of the `prelude.` qualifier added earlier the same day: `prelude.len("abc")` reaches past the import, and the two pieces of work turned out to need each other. `use bits` with `bits.len(bs)` avoids the question entirely and is what most callers should write. `tests/lang/bits_bare_import_shadows_prelude.zm` pins both halves, including that `prelude.len(bs)` on a `BitSet` is an *error* rather than a fallback into the imported one -- the qualifier names one namespace, not "try both".

Verified: `make check` green, 1,177 fixtures on both builds, and the `bits_byte`/`bits_int`/`bits_bigint` fixtures are what prove the symbol fallback dispatches to the right one of three implementations rather than to whichever `native_lookup` answered first.


## `csv` stops stuttering, and `_with` becomes an argument (2026-10-24)

`csv.parse_csv(text)` said csv twice and `csv.csv_to_string(rows)` said it three times. The 2026-10-05 developer-experience review had already written the item down -- "naming rules say qualified calls should be short verbs, but `csv.parse_csv`, `toml.parse_toml`, `yaml.parse_yaml`, `json.parse_json` stutter (verified: `csv.parse` does not exist)" -- and this closes it for `csv`. Sixteen public names become eight.

**Half the names were a workaround for a language rule, and overloading is the better answer to it.** Every reader and writer had a `_with` twin that differed only in taking a delimiter, and the module's own header comment said exactly why: they were separate functions "because Ziman can only omit an optional parameter in a call that names its arguments". That reason is still true -- confirmed by trying it, since `f(a: string, b: Option<string>)` accepts `f(a: "x")` and rejects `f("x")` -- so an `Option<string>` delimiter would have turned the common `parse(text)` into `parse(text: text)`. What changed is that a name can now carry two declarations told apart by arity, so `parse(text)` and `parse(text, "\t")` are one name and the `_with` suffix is gone from eight functions.

**`typed` went the other way, and the asymmetry is the point.** It is also an optional argument in principle, but `parse(text, true)` tells a reader nothing where `parse(text, "\t")` is self-evident, and an `Option<bool>` would cost the common call its positional form for the same reason. So the delimiter is an argument and the typing is a name: `parse` / `parse_typed`. A reader can see which of the two is which from the call alone, which is the test an optional parameter fails.

**A gap closed by itself.** `parse_csv_columns` and `parse_csv_columns_typed` -- the headerless readers -- hardcoded the comma and had no `_with` twin, so a headerless TSV could not be read at all. Threading the delimiter through `table_with_columns` was two lines once the overload existed, and `tests/lang/csv_extended.zm` now covers it.

**One thing stayed two declarations on purpose.** `format([[string]])` returns a plain `string` while `format([[string]], delimiter)` returns a `Result`, because the only thing that can fail in writing raw strings is an invalid delimiter -- and with no delimiter to validate there is nothing to report. Two declarations can differ in return type where one `Option` parameter could not, so the arity split is what keeps the no-delimiter call free of a `Result` nobody would ever check.

A bug worth recording, because of *why* it survived the first build. Converting the writer to an `Option<string>` left its body passing `delimiter` -- now an `Option<string>` -- to three functions that take a `string`, and `ziman --check` on a fixture reported nothing: the fixture failed at module resolution, on a name the rename had removed, and resolution runs **before** any body is type-checked. A clean compile of a file that names a function that no longer exists proves nothing about that function's own module. Rebuilding the interpreter and running a program that actually called it was what found it.


## Five character classes, and why they are ASCII (2026-10-24)

`is_alpha`, `is_numeric`, `is_alpha_numeric`, `is_digit` and `is_char`, each over a `string` and a `MutableString`. They were asked for as additions; what made them worth having was already in the tree.

**Six private helpers were the same function.** `csv.zm`, `toml.zm` and `yaml.zm` each held a verbatim `byte_len(c) == 1 && index_of("0123456789", c, 0) >= 0`; `regex.zm` had `c >= "0" && c <= "9"`; `mysql.zm` had `contains("0123456789", c)`; `zone.zm` had an indexed form. Three of the six were **subtly wrong and got away with it**: `regex`'s string comparison and `mysql`'s `contains` both answer *true* for `"12"`, and `mysql`'s answers true for `""` as well, correct only because no caller ever handed them two characters or none. Promoting the question to the prelude replaced all six and tightened three.

**ASCII is the decision, and the duplicated helpers are the argument for it.** Every one of the six meant ASCII: they read CSV cells, TOML bare keys, YAML scalars, regex quantifier counts, MySQL version strings and zone offsets, where `٤٢` is not a number but a syntax error. A Unicode `is_digit` would have silently changed what all six asked. The Unicode questions already have a better answer in this codebase -- `regex`'s `\p{L}` and `\p{Nd}`, backed by the real general-category tables in `src/regex_unicode.h` -- so a character-property question with a table behind it goes there, and the parsing question stays here. The alternative was considered concretely: `UNI_L`/`UNI_N`/`UNI_Nd` were already present and would have made a Unicode implementation about as cheap to write.

Being ASCII buys something beyond agreeing with the callers. **No UTF-8 decoding happens at all**: a byte of 0x80 or more is outside every class, so one byte scan answers each predicate whatever the encoding, which makes these the only members of the string family whose cost does not depend on the `ZSTR_ASCII` bit.

**`is_char` is about length, not class** -- whether the text is exactly one character -- which is what makes it the guard the others pair with. A lead byte states its own sequence length, so it needs no decode and no validation, and text that is not valid UTF-8 is simply not one character. That matches `is_char_at`, the other member that answers false rather than raising on bad bytes.

**Empty is false for all five**, which is a decision and not an oversight. "Every character is a digit" is vacuously true of no characters, and that is the mathematically tidy answer; it is also the one that would make `is_digit(c)` useless as the guard that is its entire purpose. `is_empty` is the question about emptiness.

Both worlds carry a prefixed C registration -- `string_is_alpha` and `mutable_string_is_alpha`, never a bare `is_alpha` -- because `native_lookup(fn->name)` is consulted before the world-prefix fallback, so a bare name would be found first and run the `string` implementation on a builder's handle. `char_at` and `last_index_of` both fell into that, a day apart, and it is the third time the same trap has been worth writing down.

Verified: `make check` green, 1,178 fixtures on both builds. `tests/lang/string_character_classes.zm` covers both worlds, the empty case for each, `٤٢` and `héllo` as the ASCII boundary, and a lone 0xFF built with `append_byte` -- the only route by which invalid UTF-8 can still reach a string.


## `fs` loses a hundred lines of ceremony, and gains the two functions it was missing (2026-10-24)

`fs.zm` needed no rename -- its names are already short verbs, and the one prefix it carries, `fs_error_message`, is required by `error_message`'s `<type>_message` lookup. What it had instead was repetition and two gaps, both of which the tree itself pointed at.

**Eighteen public functions were the same six lines.** Each native answers `Result<T, string>` carrying the `"category|message"` shape, and each public form converts that to `Result<T, FsError>`:

    pub fn remove(path: string) -> Result<unit, FsError> {
        val r = remove_raw(path)
        if Err(e) = r { return err(fs_error_from_raw(e)) }
        return ok(unwrap(r))
    }

written out eighteen times, differing only in which `_raw` it called -- about a hundred lines saying one thing. One generic `lift<T>(r: Result<T, string>) -> Result<T, FsError>` says it instead -- as a `switch` over the two variants, so the `Ok` arm *names* its value rather than testing for `Err` and then re-extracting with an `unwrap` that cannot fail -- and every public function became a one-line call. The `T` is what made it possible rather than clever: `unit`, `int`, `string`, `[byte]`, `[string]` and three distinct `Iter` shapes all pass through unchanged. 402 lines to 343, with no call site anywhere affected, because none of this was ever visible outside the file.

**`remove_all` was hand-rolled in the fixtures, which is how a missing function announces itself.** `make_dirs` exists (`mkdir -p`) and had no counterpart, so `tests/lang/fs_extended.zm` deleted its tree with six `fs.remove` calls in dependency order and `fs_basic.zm` with three. Both are one call now.

**It is written in C, and the reason is `lstat`.** The obvious Ziman implementation is `is_dir(p)` then recurse over `list_dir(p)`, and for a *destructive* operation it is wrong in a way worth stating plainly: `is_dir` calls `stat`, which follows symbolic links, so a link inside the tree pointing at a directory outside it would be descended into and its target's contents deleted. `lstat` answers about the link itself, so a link is unlinked and never followed -- the same care `walk` already takes. `tests/lang/fs_remove_all.zm` builds exactly that shape, with a file in the outside directory that must survive, and it does.

Two smaller decisions in it. Missing is **not** an error: a `remove_all` of a path that is not there has already achieved what it was asked, which is what makes it usable as the unguarded cleanup the fixtures want. And a failure **stops** and names the path that failed rather than carrying on, because there is no useful "mostly deleted" answer to hand back.

**`temp_dir()` replaces a hardcoded `/tmp`** that `process.output` was building paths under. That is wrong twice: a system may put the writable temporary directory elsewhere and say so through `TMPDIR`, and a sandbox may make `/tmp` unwritable. It answers a directory rather than a unique path inside one, since the uniqueness ingredient (`os.pid()`) belongs to the caller.

**What was deliberately not done: `write_text`/`write_bytes` were left alone.** They could be one `write` told apart by the argument's type, which is the rule the rest of this week's renames moved under. The reads cannot -- `read_text` and `read_bytes` differ only in return type -- so collapsing the writes would leave `fs.read_text` beside `fs.write`, and the asymmetry between a pair that reads and writes the same thing is worse than the prefix. A rule that would break a symmetry is the wrong rule to apply there.

Found while verifying: `process.run`'s parameters carry default values (`vars: Option<[EnvVar]> = None`), and a default **still** does not make a parameter positionally omittable -- `process.run("ln", args)` is rejected for want of six arguments. Only an all-named call may omit one. That is the same constraint `csv` ran into earlier the same day, now confirmed to hold for defaults as well as for bare `Option`s.


## `json` stops stuttering, and derive learns to call it qualified (2026-10-24)

`json.parse_json(text)` and `json.json_to_string(j)` said json twice; `json.json_object_get` said it twice and buried the question. Twenty names lost the prefix and 424 references moved with them, which closes for `json` the same review item `csv` closed earlier today.

**Six names became three overload sets**, because the argument already says which is meant: `parse`/`parse_strict`/`parse_exact` each take a `string` or a `[byte]`, where the `_bytes` suffix used to say it. `json_to_string` and `json_to_pretty_string` became one `format`, told apart by arity -- the indent is an argument, not a name.

**Three of the new names are the prelude's, and they are reached qualified for that reason.** `format`, `get` and `to_bytes` all exist in the prelude, and a per-name import *replaces* a bare name for the whole file rather than extending the overload set -- so `use json.get` would cost that file the prelude's `get` on a `Map`. Measured before deciding: 22 files bare-imported `json_to_string`, 12 `json_object_get`, and nine of them use a prelude `get`/`format`/`to_bytes` in the same file. So those three are never bare-imported; the sweep rewrote their call sites to `json.format(x)`, `json.get(j, k)` and `json.to_bytes(j)`, which is the spelling the `bits` entry already recommends as the default. The other seventeen are ordinary bare imports.

**`#[derive(Json)]` had to change with them, and in a way that is strictly better than before.** Derive generates calls to `json_object_get`, `json_require_field` and `json_require_string_field` and auto-imports those names into the deriving file. Under the new names that import would have been `use json.get` -- silently shadowing the prelude's `get` in any file containing a `#[derive(Json)]`, without the author asking for it or being able to see why. So derive emits the qualified `json.get(...)`/`json.require(...)` and imports the *module*, which cannot shadow anything. A fixture now derives in a file that also calls the prelude's `get` on a `Map`, which would have failed either before this change or after a naive version of it.

**What kept its prefix, and why it is the same rule as before.** The scalar converters -- `int_to_json`, `string_from_json`, `decimal_from_json` and the rest, plus `json_to_json`/`json_from_json` -- are the derive *convention*, generated by name from the field's type. `string_from_json` could only become `to_string` by changing that convention for every derived type in every program, which is the change the 32-candidate overload cap already ruled out for `to_json`. They are the bridge; everything else is vocabulary.

`require_bigint` and `require_decimal` fill the two holes in the `require_*` family: a required field could be read as an `int`, `float`, `string` or `bool`, but a whole number past `int`'s range and an exact decimal both had to go through `require` plus a hand-written `bigint_from_json`/`decimal_from_json`.

Two corrections made while writing the `require_decimal` doc, both worth keeping. The obvious example of float imprecision, `19.99 + 19.99`, **is not one**: the nearest double to 39.98 is exactly the sum of the two nearest doubles to 19.99, so both paths print `39.98` and shortest-round-trip display hides the difference entirely. `0.1 + 0.2` is where it shows -- `0.30000000000000004` through floats, `0.3` through decimals -- and the fixture says so rather than asserting a difference that is not there. The first draft of the comment claimed `require_float` of `19.99` "is not 19.99", which a reader could disprove in one line.

A mechanical lesson from the sweep itself, which cost a full suite run. Rewriting `json.<old>` to `json.<new>` with a word-boundary regex **also rewrites the `use json.<old>` line**, so the pass that decides how the body should be spelled saw no import, left every bare call alone, and produced 56 failures in files whose imports were already renamed. Handle the `use` lines first, then the body. A second miss followed from the opposite direction: a bare-call pattern anchored on `name(` skips `name<T>(`, so four generic call sites survived to be caught by the North Star build rather than the suite.


## `jwt`: one `sign`, one dead function gone, and algorithm pinning (2026-10-24)

`jwt.zm` needed almost nothing renamed -- `jwt.sign`, `jwt.verify`, `jwt.claims`, `jwt.subject` are already short verbs, and its one prefix, `jwt_error_message`, is required by `error_message`'s `<type>_message` lookup. Three things were worth doing, and one thing deliberately not.

**`sign_with` became a third argument to `sign`**, told apart by arity. The suffix was saying what the argument already says, and an algorithm is a `string` where nothing else in that call is: `sign(claims, key, "HS512")` needs no name to be read, which is the test `csv`'s `typed` flag failed and this passes.

**`verify_leeway` is gone, with zero callers anywhere in either repository.** It was `verify_claims(leeway_seconds:)` under another name. Measured before deleting rather than assumed.

**The verify family was *not* collapsed further, and the reason is a language constraint worth recording.** The tempting shape is one `verify` covering `verify(token, key)` and the all-named `verify_claims(token:, key:, issuer:, ...)`. It cannot exist: **overloading a name disables `Option` omission**, because the resolver filters candidates by argument *count* before any Option rule applies. A two-member set of `v(token, key)` and `v(token, key, leeway: Option<int>, now: Option<int>, strict: Option<bool>)` rejects `v(token: "b", key: 2, leeway: 30)` with "no declaration of 'v' takes 3 argument(s)". So a function that relies on omitting optional parameters must be the *only* declaration of its name, which is exactly why `verify_claims` keeps one. That is the third Option finding in a day: `csv` found omission needs an all-named call, `fs` found a default value does not change that, and this finds overloading removes it entirely.

Nor was `verify_at` folded into `verify` by arity, which *would* have worked. `verify(token, key, 30)` cannot be read -- is 30 a leeway or a Unix time? -- where `verify_at(token, key, now, leeway)` says. The same judgement as `parse`/`parse_typed`: an argument carries a name when the value alone does not identify it.

**Algorithm pinning, added as an opt-in and described honestly.** `check` reads `alg` from the token's own header and verifies under it. `verify_claims` now takes `algorithm`, compared against the header *before* the MAC is computed, so a deployment signing HS512 can refuse a valid HS256 token over the same key.

What this is not is a vulnerability being closed, and the entry says so because the opposite claim would be easy and wrong. The attack that makes a trusted `alg` header dangerous needs a **mix** of symmetric and asymmetric algorithms -- presenting an RS256 public key as an HS256 secret. This module's whole algorithm set is HS256, HS384 and HS512, so switching `alg` buys an attacker nothing: every one of them still needs the key to produce a matching MAC. Audited alongside it and found already correct: the signature comparison goes through `crypto.bytes_equal`, which is constant-time by contract, `alg: none` falls out as `UnsupportedAlgorithm` because `mac` has no such arm, and the payload is decoded only *after* the signature verifies.

A pin cannot be used to weaken a check either, which the fixture pins: pinning HS256 against an HS512 token is refused, because the comparison is against what the header claims rather than a floor.

**Found in the sample backend, and fixed there.** `verified_claims` called `verify_claims` *positionally*, with three explicit `None`s for the checks it did not want -- which says nothing about what is being left out and broke the moment an eighth parameter arrived. It names its two arguments now, which is what the all-named form is for and what would have survived the change untouched. A positional call into a function whose tail is all `Option` is a latent break, not a style preference.


## `log`: the `Level` family loses its prefix, and `format` stops padding (2026-10-24)

`log.zm` is another module whose names were already right -- `log.info`, `log.logger`, `log.enabled`, `log.with_prefix` -- with one family left over and one wart inside it.

**`level_rank`, `level_name` and `level_parse` named the type in two of the three cases where the argument already does.** `rank(level)` and `format(level)` take a `Level`, so they go bare; `parse_level(text)` takes a `string` and keeps the type in its name, which is the same split that gave `time` its `parse_localdate` and `csv` its `parse`. Four call sites outside the module, all of them `level_parse`.

**The wart is the more interesting half: `level_name(Level.Info)` answered `"INFO "`.** The padding is five characters so the text format's columns line up, and it was added deliberately -- `tasks.md`'s F41 records noticing it, and notes that `json_line` therefore has to `trim` it back off before the level reaches a log index. That is the whole argument against it: a presentation detail in the value, which one caller wanted and every other caller had to undo. Anyone writing their own format inherited a stray space they could only find by printing it.

So `format(level)` answers the bare name now and a private `padded_name` does the column work, called from the one place that has columns. `json_line` lost its `trim`, and the text lines are byte-identical -- `log_basic.out` and `log_json_lines.out` gained assertions and changed nothing else, which is what makes this a refactor rather than a behaviour change.

`padded_name` needs no general pad helper and does not pretend to be one: `Level` is a closed enum of four whose longest name is five characters, so one comparison does it. A `pad_right` in the prelude would be the wrong answer to this, and there is no second caller asking for one.

Nothing else here wanted changing. `with_prefix` and `with_request_id` have identical signatures (`Logger`, `string`) and so could not be one overload set even if that read better, which it would not -- the names say which field. And the four level helpers (`debug`/`info`/`warn`/`error`) are the convenience surface over `write`, not variants to collapse.


## `markdown` drops the stutter, and the whole module was already qualified (2026-10-24)

`markdown.markdown_to_string(doc)` said markdown twice and `markdown.parse_markdown(text)` said it twice. Six names become four: `format` over a `Document` and a `Block` (two declarations, told apart by the argument -- `block_to_string` was the second), `to_bytes`, and `parse`/`parse_strict` with `parse` taking a `string` or a `[byte]` where the `_bytes` suffix used to say it.

**The thing worth recording is how cheap it was, and why.** Every one of the nineteen call sites in both repositories was already written `markdown.X(...)`, with **zero** per-name imports -- so none of the shadowing analysis `json` needed applied here. `format` and `to_bytes` are prelude names, and because nothing bare-imports them from this module, nothing in either repository could lose the prelude's own. The only file that needed `prelude.` was `markdown.zm` itself, for its own ten interpolating `format` calls and one `to_bytes`.

That is the pattern across the five modules renamed today: the cost of taking a short name is paid entirely by files that *bare-import* it, and the modules that were always called qualified cost nothing at all. It is also an argument for the qualified style as the default, which `bits` and `json` both now say outright in their own documentation.

Nothing else in the module wanted changing. `text_table` and `text_list` keep their prefix, which is not the type but the *input*: they build a `Block` from plain `string`s where the richer builders take `[Inline]`, and `table(headers, alignments, rows)` would hide that distinction rather than shorten it.


## `math`: the decimal roundings join their float namesakes, and answer a `bigint` (2026-10-24)

`math.zm`'s names were already bare verbs -- `abs`, `sqrt`, `floor`, `round` -- with one family carrying a type it did not need to. `decimal_round(d, scale)` takes a `decimal` where `round(x)` takes a `float`, which is exactly what overload resolution reads, so `round`, `floor`, `ceil` and `pow` each became one set and the `decimal_` prefix went.

**The file said this could not be done, and the reason it gave was wrong.** SYNTAX.md's decimal section had `decimal_round` as "a separate name rather than a `round` overload: the two would take the same argument types and differ only in what they return, which no argument list can tell apart". That sentence is true of `round` and `round_float` -- both take a `float` -- and had been transplanted onto a pair it does not describe. Checked by building it rather than by re-reading the claim: `round(2.5)` is `3`, `round(1.235d, 2)` is `1.24`, and `round(2, 3)` is a clear "argument 1 expects decimal, found int" rather than an ambiguity, because there is no implicit int-to-decimal conversion to make one.

**`floor`/`ceil`/`round` of a `decimal` answer a `bigint`, not an `int`.** A `decimal` is arbitrary-precision, so its whole part can exceed 2^63 and an `int` would have to overflow or lie; `math.floor(99999999999999999999.9d)` is a case the fixture pins. The `float` members answer an `int` honestly because a `float` cannot hold a whole number that large to begin with. Computed as the scale-0 rounding followed by an exact conversion, so the floor/ceil/half-away choice is made by the decimal rounding and not quietly re-made by `decimal_to_bignum_trunc`.

**What this cost in the interpreter, and the shape of it.** Each of those names now has two native implementations, and `native_lookup(fn->name)` runs before the world-prefix fallback -- so the bare `math::round` registration had to go, or it would have been found first and run the float code on a decimal. That is the third time this trap has come up (`char_at`, `last_index_of`, now here), and the fix is the same: register `math::float_round` and `math::decimal_round`, and let the fallback derive which from parameter one's type. The fallback gained `float_` and `decimal_` prefixes, completing the scalar set `bits` started this morning with `byte_`/`int_`/`bigint_`.

The fallback reads **parameter one only**, which matters here: `floor(d)` and `floor(d, scale)` are both a `decimal` and so land on the same symbol. One C function dispatches on `argc`, the way `to_upper`'s two arities already do -- the alternative would be teaching the fallback about arity, for one caller.

`abs`, `min` and `max` keep their bare registrations and need none of this: one C function serves all of `abs`'s four declarations by inspecting the value, so there is nothing for the fallback to tell apart.

**`floor_float`/`ceil_float`/`round_float` keep their suffix, and the module now says why.** They take the identical `float` argument as the members above and differ only in answering a `float` rather than an `int`, which no argument list can state -- the same constraint that keeps `fs.read_text` and `fs.read_bytes` apart. It is worth having written down beside them, because the next reader will otherwise try exactly what I just did to the `decimal` family and find out the hard way.


## `math` gains five: `is_finite`, `is_infinite`, `log2`, `log10`, `sign` (2026-10-24)

Chosen from a longer list by asking what the tree already hand-rolls, which has been the reliable signal: a promoted helper gets checked against its existing callers, and a function with no caller gets no such check.

**`is_finite` and `is_infinite` were already being written inline.** `json.zm` spelled `f != f || f - f != 0.0` in both `bigint_from_json` and `decimal_from_json`, and `pdfobj.zm`'s `real_text` guards `x != x` with a comment noting that the backends refuse non-finite geometry elsewhere. `is_nan` was public and these two were not, which is an odd place to stop: the three are one question asked three ways. Both are pure Ziman over `x - x`, which is `0.0` for any finite `x` and NaN for an infinity -- and NaN for NaN, which is why NaN is excluded first and why the expression deserves a name rather than a repeat appearance. `json.zm` asks `math.is_finite` now, which also gave `json` its first dependency on `math`; no cycle, `math` imports nothing.

**`log2` and `log10` are a correctness argument, not a convenience one.** Without them a caller writes `log(x) / log(2.0)`, and that is measurably worse: libm's versions are exact for an exact power of their base where the division is not. `log10(1000.0)` is `3.0`; `log(1000.0) / log(10.0)` is `2.9999999999999996`; and `floor` of the two is **3 and 2**, which is exactly how a digit-counting loop ends up off by one. The fixture prints all four numbers rather than asserting the claim, because the claim is the reason the functions exist.

**`sign` answers an `int` for all four numeric types**, one C function inspecting the value as `abs` already does, so it needs no per-type symbol. A `BigNum` carries its own `sign` field and a `Decimal` is a `BigNum` plus a scale, so both are a field read rather than a comparison.

**NaN is a runtime error from `sign`, and that is the decision in it.** The answer has to be an `int`, and every candidate is a lie: `0` would make `sign(nan)` agree with `sign(0.0)`, and -1 or 1 would claim an ordering NaN does not have. Java returns NaN from `signum` because its return type can hold one; an `int` cannot, so the honest options are an error or a `Result`, and an error that names `is_nan(x)` is the cheaper of the two for a function nobody wants to unwrap. Both infinities have a perfectly good sign and get it.

**What was left out, and why.** `clamp` is `min(max(x, lo), hi)` and the one nested `min`/`max` in the tree is an edit-distance recurrence, not a clamp. `gcd`/`lcm`, `cbrt`, the hyperbolics and `to_radians`/`to_degrees` have no caller anywhere -- including in `lib/graphics`, which was the place to look for the trigonometric ones and does not want them. `hypot` and `trunc` are the two with a real argument behind them (overflow-free magnitude; the fourth rounding mode, with `decimal_to_bignum_trunc` already in C) and are the obvious next pair if anything asks. Float limits are already `number.float_min`/`float_max`/`float_min_positive`, so they are placed, just not here.


## `metrics`: fifteen names become five (2026-10-24)

Every recording operation came in three forms, and the suffixes only restated the argument list. `increment(name)`, `increment_with(name, labels)` and `increment_at(series)` differ in exactly what their arguments already say -- a `Map` third argument is self-evidently a label set, a `Series` first argument self-evidently a resolved one -- so the five operations (`increment`, `add`, `set`, `gauge_add`, `observe`) are five overload sets of three, and ten names are gone. `plain_series(name)` folded into `series` the same way, by arity.

**All 43 call sites were already qualified, with zero per-name imports**, which is what made this safe to do at all: `add` and `set` are both names the prelude has -- `set` is even an interpreter-dispatched builtin -- and a bare `use metrics.add` would have cost that file the prelude's `add` on a `Set`. Nothing imports them that way, so nothing had to change. The same observation as `markdown` an hour earlier, and the third module today where the qualified call style made a rename free.

**Six of the eighteen functions had no caller anywhere**: `add_with`, `set_with`, `observe_with`, `gauge_add_with`, `plain_series` and `set`. Folding them into overloads keeps the capability while removing the name, which is the right answer for a metrics library -- labelled recording is real API that a program will want even if this tree's two programs do not. Deleting them would have been the wrong read of "no caller".

**`series_of` kept its own name, and this is the one judgement in the change.** Its argument types *would* tell it apart from `series(name, labels: Map)` -- a `string` second argument against a `Map` one -- so overloading it was available and I did not take it. It accepts a pre-built label key and its own doc says getting that key wrong "splits a series rather than failing": a silent wrong answer, not an error. As a third `series` overload the unsafe path becomes reachable by typo; as a separate name it is reachable only on purpose. Same rule as `csv`'s `parse_typed` and `jwt`'s `verify_at`: a name carries what an argument cannot, and here what it carries is which of two paths you are accepting responsibility for.

A process note on the mechanics. Moving `series_of` below the second `series` declaration -- so the set's two members sit together -- was done with a script that searched for the next `}\n`, and that matched the brace closing `Series { handle: ... }` on the `return` line, nesting a `pub fn` inside a function body. 29 fixtures failed, the parse error pointed at the right line, and the fix was to stop scripting a two-line move. Worth recording because the first instinct on a 29-failure diff is to look for something subtle.


## `mime` was already right, and `to_string` turns out to be library-wide (2026-10-24)

`mime.zm` needed one rename: `to_string(m)` became `format(m)`, joining the spelling `time`, `csv`, `json`, `bits`, `markdown` and `log` all moved to earlier today. Ten call sites, every one qualified.

**The rest of the module was checked and left alone, which is the finding worth recording.** Three things were looked for and not found:

- **Bare media-type literals.** 35 occurrences of `"application/json"`-shaped strings exist in `lib/` and the backend, but 29 of them are inside `mime.zm`'s own extension table and 5 of the remaining 6 are in *test* files asserting an exact header value -- where a literal is the right thing, since the test exists to pin the string. The sixth is a comment. So nothing is reaching around this module.
- **Content negotiation.** `matches(pattern, candidate)` has no quality-value sibling, and an `Accept:` parser would be the obvious addition -- except that nothing negotiates: `client.zm` sends `Accept: */*` and the server never reads one. `Accept-Encoding` *is* parsed in `server.zm`, but its entries are encodings rather than media types, so `mime` is not their home.
- **Table against constructors.** The extension table is *ahead* of the seventeen well-known constructors, not behind: `wasm`, `js`, `css`, `tsv`, `md`, `webp`, `ico`, `woff2`, `mp3` and `mp4` all resolve through `of_extension` with no named function of their own. Adding ten more zero-argument functions is surface for its own sake; `of_extension("css")` already answers.

**What the rename did turn up is bigger than `mime`.** `to_string` is a residue across six modules, not one: `uuid.uuid_to_string`, `toml.toml_to_string`, `ndjson.ndjson_to_string`, `yaml.yaml_to_string`, and `xml.xml_to_string` / `xml_to_string_pretty` / `element_to_string` -- which are three declarations that would collapse into one `format` set the way `markdown`'s `Document`/`Block` pair did. Four of the six also stutter (`toml.toml_to_string`). Recorded here rather than swept, because each is its own module's turn; `json.to_string_map` is *not* one of them, being "to a map of strings" rather than a display form, and neither is the prelude's `bytes_to_string`.


## The `to_string` residue, cleared in one pass across five modules (2026-10-24)

`mime`'s rename an hour earlier turned up that `to_string` was a library-wide spelling, not a one-module leftover. Five modules carried it, four of them stuttering as well, and the `parse_*` stutters sat beside them -- the same recorded review item `csv` and `json` closed earlier. So both directions moved together per module, because `toml.parse_toml` beside `toml.format` would be a worse state than either end of the change.

| module | was | is |
|---|---|---|
| `toml` | `parse_toml`, `toml_to_string` | `parse`, `format` |
| `yaml` | `parse_yaml`, `yaml_to_string` | `parse`, `format` |
| `ndjson` | `parse_ndjson`, `ndjson_to_string` | `parse`, `format` |
| `xml` | `parse_xml`, `_with`, `_bytes`, `_bytes_with`, `xml_to_string`, `_pretty`, `element_to_string` | `parse` (4), `format` (3) |
| `uuid` | `uuid_to_string`, `uuid_from_string`, `uuid_version` | `format`, `parse`, `version` |

**`xml`'s seven names became two overload sets**, which is the clearest case the whole week produced: `parse` over `(string)`, `(string, XmlLimits)`, `([byte])` and `([byte], XmlLimits)` -- where `_with` and `_bytes` were suffixes restating the arguments -- and `format` over `(Document)`, `(Document, string)` and `(Element)`.

**`uuid` was the one that needed the shadowing analysis.** The other four are never bare-imported, so their short names cost nothing; `uuid_to_string` was bare-imported in 29 files and `uuid_from_string` in 7, and ten of those files also use the interpolating `format`. So `format` and `parse` are qualified-only for `uuid`, with the 59 call sites rewritten to `uuid.format(...)`/`uuid.parse(...)`. Its constructors keep their prefix (`uuid_new`, `uuid_new_v7`, `uuid_nil` -- nothing but the return type distinguishes them) and so do the compiler-bound `uuid_to_json`/`_from_json`/`_to_sql`/`_from_sql`/`_compare`. `uuid_version(u)` went bare, its argument saying the type.

**Four mechanical failures, all from scripting what should have been read.** They are worth listing because each is a different edge of the same mistake:

1. **A comment counted as a use.** `lib/db/db.zm` mentioned `uuid_from_string` only in a doc comment; the script rewrote it, concluded the module was used, and added `use uuid` -- which is a *cycle*, since `uuid` imports `db` for `SqlValue`. 77 fixtures failed on "circular 'use' involving module 'db'". The fix strips comments before asking whether a module is really used.
2. **A module's own new name collided with an import it already had.** `ndjson.zm` bare-imported `json.parse` (from the json rename hours earlier) and now declares its own `parse`: "'parse' is imported here but also declared in this module". `json`'s is qualified there now.
3. **Dedupe across a Markdown file is wrong.** The same `use` line legitimately repeats in independent code blocks, and a whole-file dedupe deleted `use json.Json`, `use math`, `use datasource` and `use net` from four examples. Hand-written docs get substitution only -- no `use`-line manipulation, no dedupe.
4. **`docs/reviews/` is not uniformly historical.** Excluding it wholesale left `benchmark/reference/33-toml-config.zm` and `36-yaml-list.zm` on dead names, and those two are *compiled and run* by `benchmark-check` -- unlike `benchmark/runs/`, which is recorded output. The distinction had been got right for `bits` that morning and lost here.

`json.to_string_map` is deliberately untouched: it converts to a `Map<string, string>` rather than producing a display form. So is the prelude's `bytes_to_string`.


## `net`: the `_with` suffixes go, and three names are kept on purpose (2026-10-24)

`net.zm` had the `_with`/`_with_options` explosion `csv` had: `start_tls`, `start_tls_with`, `start_tls_with_options`, and the same shape for `connect_tls` and `listen_tls`. Each suffix restated the type of an argument the call already carries, so five names collapsed into three overload sets, `lines_with` folded into `lines` by arity, and `close_listener` became the `Listener` member of `close`. Seven names gone, 75 references moved, every call site already qualified.

**The judgement in this one is what was *not* collapsed, because overloading would have worked and was still wrong.**

`connect_unix` and `listen_unix` take a single `string` and would resolve against `connect(host, port)` by arity. They name a different *transport*, not a different arity, and `connect("/tmp/app.sock")` reads as a URL with a missing port rather than as a Unix socket.

`connect_timeout(host, port, 5000)` keeps its name because a bare trailing `int` says nothing -- the same test `csv`'s `typed` flag failed and `csv`'s delimiter passed. `connect(host, port, 5000)` could as easily be a retry count.

`connect_tls`/`listen_tls` stay separate from `connect`/`listen`, and this is the one with a real cost: both pairs differ in arity and *would* resolve. The argument against is that whether a connection is encrypted is the last property anybody should infer from counting arguments. A reader scanning for the plaintext listener wants to grep `net.listen(`, and if `net.listen(host, port, options)` were the TLS one, that grep would answer wrongly. The suffix is doing security-relevant work, not naming a type.

`read_text`/`write_text` could not have joined `read`/`write` in any case: they take the identical arguments and differ only in answering a `string` rather than `[byte]`, which no argument list can state -- the third module this week to hit that (`fs.read_text`, `math.floor_float`, now here).

The `Option` in `start_tls_with(s, host, verify, ca_file: Option<string>)` needed checking before the fold, since **overloading a name disables `Option` omission** (the `jwt` finding). Every one of its eight call sites passes all four arguments explicitly, so nothing depended on omitting it.

Verified including the TLS integration suites, which carry 21 of the edits and are the only place the handshake paths run: `tls_alpn`, `tls_behavior`, `tls_client_certificate`, `tls_close_while_reading`, `tls_server`, `tls_verify` and `httpclient_tls` all pass.


## `number`: one `format`, one `to_hex`, and the `bytes_` world the fallback was missing (2026-10-24)

Two families collapsed and two deliberately did not, and the interesting part is in the interpreter.

**`format_int`/`_float`/`_bigint`/`_decimal`/`_byte`/`format_number` became one `format`** of six members, and `parse_number` became `parse`. Each takes its type plus a `NumberFormat`, so the argument says which. SYNTAX.md had argued against exactly this -- "**One engine, not five** (one function, not an overload set per numeric type)" -- and the argument was sound when written, because overloading did not exist then. It still reads as one engine: the `string` member *is* the implementation and the five typed ones are thin wrappers over it; what changed is that they share its name.

**`byte_to_hex`/`int_to_hex`/`bytes_to_hex`/`bigint_to_hex` became one `to_hex`** of four. Their readers could not follow: all four take a `string` and differ only in what they answer, so `byte_from_hex`, `int_from_hex`, `bytes_from_hex` and `bigint_from_hex` each keep their type. That asymmetry has now appeared in `time` (`format`/`parse_localdate`), `json` (`to_json`/`to_int`), `fs` (`read_text`/`read_bytes`), `math` (`floor`/`floor_float`), `net` (`read`/`read_text`) and here -- six modules, one rule, and it is worth stating as the rule rather than as six coincidences: **a writer can share a name because its argument names the type; a reader cannot, because its argument is always the serialised form.**

**`float_to_text` was kept out of `format`**, although `format(x: float)` would have resolved beside `format(x, fmt)` by arity. `format` renders *for a reader* -- grouping, decimal mark, fixed places, all from a `NumberFormat`. `float_to_text` renders for a *machine*: the shortest text that reads back as exactly the same float, no locale anywhere in it. One name would invite `number.format(x)` to be read as "format with the defaults", which is precisely what it is not. (It is also not `str(x)`, which keeps six significant digits.) The byte-packing family -- `u16_to_bytes`, `u32_to_bytes`, the six `*_from_bytes` -- keeps every name for the plainer reason that width and signedness are not in the argument list at all.

**The interpreter needed a `bytes_` world, and finding out why took a runtime error.** `to_hex`'s four declarations are all named `number::to_hex`, so the symbol fallback derives one per parameter type -- and for `[byte]` it derived `number::array_to_hex` against a registration of `number::bytes_to_hex`. The declarations compiled, the suite passed, and `number.to_hex([1b, 2b])` failed at runtime with "no registered implementation": the third time this week that a native's *declaration* and its *symbol* came apart invisibly, and the second where only running it found out.

So the fallback now maps an array of `byte` to `bytes_` and every other array to `array_`, which is what this library calls that world everywhere else (`to_bytes`, `bytes_to_string`, `bytes_from_hex`, `bytes_equal`). That broke `index_of` on `[byte]` -- 25 fixtures, all through `http.client` -- because its registration was `array_index_of`, standing in for the `bytes_` world that had not existed. `index_of` has exactly three members (`[byte]`, `string`, `MutableString`) and no generic `[T]` one, so that symbol only ever served the byte member: it is `bytes_index_of` now, which is what the C function has always been called. Checked exhaustively rather than guessed -- the prelude has only two `[byte]`-first natives, and the other (`bytes_to_string`) is registered bare and never reaches the fallback at all.

Found while fixing the docs: an earlier sweep today had left a spurious `use number` in SYNTAX.md's decimal example, above `use math` and so out of the order `--fmt` sorts imports into. The syntax checker caught it, which is the second time that check has paid for itself today.


## `number`'s patterns become real `DecimalFormat` patterns (2026-10-24)

Three changes, each closing a gap the previous entry had only described.

**`0` and `#` now mean different things, which is the one piece of real `DecimalFormat` semantics that was missing rather than merely absent.** After the point a `0` is a mandatory place and a `#` an optional one, so `#,##0.00` always writes two and `#,###.##` writes at most two with trailing zeros dropped -- `1,234,567.50` against `1,234,567.5`, and for a whole number `1,234,567.00` against `1,234,567`, the point disappearing entirely as it does in Java. In the integer part the `0`s are a minimum width, so `00000` writes 42 as `00042` and -42 as `-00042`, the sign outside the padding. A `0` after a `#` in the fraction is rejected, Java's own rule: a mandatory place cannot follow an optional one.

That needed the struct to grow. `decimal_places: Option<int>` could only say "exactly N", which is why the two patterns formatted identically; it is `min_decimal_places: int` plus `max_decimal_places: Option<int>` now, with `min_integer_digits: int` beside them. Each field says one thing, where an exact-or-nothing count said less than the pattern did.

**`parse_pattern` answers a `Result`.** The old reader split the pattern on `.` and read the pieces by position, so `#.##0.00` -- a plausible thing to write, being what a European *rendering* looks like -- silently produced no grouping and three decimal places, and `"hello.xx"` produced a format too. Every malformed pattern now names its problem: "more than one decimal point", "'h' is not allowed in the integer part", "a decimal point with no '0' or '#' after it". A pattern is a literal at every existing call site, so each became `unwrap(parse_pattern(...))`, which is the honest spelling for a constant that can only fail through a bug.

Making it a `Result` rather than adding a checked sibling was the choice: two readers of one pattern language, one of which quietly guesses, is the kind of pair DESIGN's second principle exists to prevent.

**`parse_pattern(pattern, locale)` combines the two halves `DecimalFormat` keeps separate.** Before this, neither call could express "two places, German symbols": a pattern baked in `,` and `.`, and `number_format_for(locale)` gave a locale's separators but no way to force a place count. The pattern gives the shape and the locale gives the symbols, which is exactly `new DecimalFormat(pattern, symbolsFor(locale))`. `parse_pattern("#,##0.00", locale.of("de"))` formats 1234567.5 as `1.234.567,50`. Grouping stays off if the pattern never asked for it, so `parse_pattern("0.00", de)` is `1234567,50` -- the locale supplies a separator but does not turn grouping on.

The composition by hand still works and is pinned by a fixture, because the fields are assignable: `with unwrap(parse_pattern("#,###.00")) { thousands_separator: ".", decimal_separator: "," }`. What changed is that nobody has to.

Still unsupported, and still deliberately: `%`, `E`, `;`-separated negative subpatterns, literal prefix and suffix text, and custom grouping sizes -- grouping is always by 3, and a `,`'s position in the pattern is not read. Truncation also remains truncation: `0.##` of `1.999` is `1.99`, because rounding is `math.round`'s job and doing it here would hide which of the two happened.


## `os`: three small things, and the fourth time a native's symbol came apart (2026-10-24)

`os.zm` is 176 lines of short verbs -- `pid`, `args`, `cwd`, `exit`, `hostname`, `cpu_count` -- with three exceptions.

**`os.os_name()` and `os.os_arch()` stuttered**, and are `os.name()`/`os.arch()`.

**`getenv` and `set_env` were an inconsistent pair**, which is the kind of thing that only shows up when you read them on one line. `getenv` is C's spelling; `set_env` is *not* C's (that is `setenv`), so the two agreed with neither convention -- one borrowed from C and the other did not, in the same two-function family. Both are snake_case now: `get_env`/`set_env`. Not folded into one name, obviously: a read and a write under one spelling would be a different kind of mistake.

**`cpu_usage_between(before, after)` is a member of `cpu_usage`**, since two `CpuTimes` arguments already say "between two readings". Its two members answer different types -- a plain `float` here, a `Result<float, string>` for the sampling form -- and that is the same principled asymmetry `csv`'s `format` has: this one is arithmetic over readings the caller already holds and cannot fail, where sampling has to read the counters and can. A `Result` nobody could ever check would be worse than the difference.

**The thing worth writing down is the native-symbol trap, now on its fourth appearance this week.** Renaming the declaration `os_name` to `name` left the registration at `os::os_name`, and `os.name()` failed at runtime with "no registered implementation" -- the declarations compiled and the suite passed, as always with this one.

What is new here is *why the fallback could not save it*. For `to_hex` and `bits`'s operators the fallback derived the right symbol from parameter one's type once the worlds existed. `name()` and `arch()` **take no arguments**, so there is no parameter to read and the fallback is skipped outright (`native == NULL && fn->params.count > 0`). A zero-argument native's registration has to carry its declared name exactly, and no amount of world-prefix machinery will cover it. That is now a comment beside those two registrations.

Four instances, three distinct causes: a bare registration found before the fallback (`char_at`, `last_index_of`), a world the fallback did not have (`to_hex` on `[byte]`, `bits`'s scalars), and no parameter to derive from (here). The common thread is that **a `native fn`'s name and its symbol are two different things that only a run can prove agree** -- not the build, and not the suite unless a fixture happens to call that exact member.


## `otp`: `digits` becomes real, and the verifiers learn to count (2026-10-24)

`otp.zm`'s names needed nothing. `totp_valid` and `totp_matching_step` keep their `totp_` prefix for the reason `net`'s `connect_tls` keeps its: it names the *algorithm*, and a reader verifying a code needs to know whether it is time-based or counter-based before anything else. The implementation needed nothing either -- RFC 4226's and 6238's own vectors pass, the comparison goes through the constant-time `crypto.bytes_equal`, and `totp_matching_step` deliberately checks every step in the window *after* a match so the time taken cannot say which step matched. That last one is careful work and worth not disturbing.

**What was wrong was `digits`.** The doc said "6 to 9" and nothing enforced it, so three things happened that should not:

- `digits: 10` answered a ten-digit code. Dynamic truncation (RFC 4226 §5.3) yields a 31-bit number, at most 2147483647, so the tenth digit could only ever be 0, 1 or 2 -- a code that *looks* stronger than it is, which is the worst direction for this kind of mistake.
- `digits: 0` and `digits: -1` both answered the single character `"0"`.
- `digits: 20` reached an integer overflow in the modulus loop and reported *that*: "integer overflow in '*'" at a line inside `otp`, naming nothing the caller wrote.

It is a runtime error naming the bound now, which is what `bits`'s shift amount and `decimal_pow`'s exponent already do for the same shape of mistake.

**And the verifiers could not check an eight-digit code.** `totp` hardcoded six. `hotp(key, unix_seconds / 30, 8)` produced one by hand -- which is how the existing fixture tested the eight-digit path -- but `totp_valid` and `totp_matching_step` both went through the six-digit `totp`, so a deployment using eight digits (RFC 6238 permits it, and its own Appendix B test vectors *are* eight) could generate codes it had no way to verify. All three take an optional `digits` now, six when omitted.

That gap is why the new fixture pins RFC 6238's vectors rather than a round trip: all five of them are eight digits, so before this change the suite could not have contained them in the form the spec states. The six-digit RFC 4226 vectors are in the same fixture as the thing that must not have moved.

**A side effect worth keeping.** Making `totp_valid` an overload set switched on the dropped-result check for it, and caught SYNTAX.md's own example discarding the answer. A pure function whose entire purpose is to tell you whether a code is valid is exactly the one where throwing the result away should not compile; the documentation now says so, and demonstrates binding it.


## A new module: `encoding`, and the two families inside base-N (2026-10-24)

Base-N encodings were in three modules: base64 in `crypto`, base32 in `otp`, hex in `number`. Each had got there for a local reason -- base64 sits beside the OpenSSL work, a TOTP secret *is* base32, hex belongs with number formatting -- and every one of those reasons was true and none of them survives the question "where do I go to encode some bytes". `lib/core/encoding.zm` is where, with base 2, 8, 16, 32 and 64 plus `base64url`.

**The design content is that the five are not one family.**

`base2`, `base8` and `base16` are **fixed width per byte**: 8, 3 and 2 digits, each byte rendered on its own. No bit packing, no padding, no alignment question -- the text length is always the byte count times the width, and a cut on any digit boundary still decodes.

`base32` and `base64` are **RFC 4648 bit packing**: 5 or 6 bits taken across byte boundaries, so the length is not a simple multiple and the last character carries leftover bits. These two are the ones the RFC defines and the ones that interoperate with anything else.

That distinction is not cosmetic. It is the difference between a string you can split anywhere and one you cannot, and a reader who thinks of the five as a uniform family will eventually cut a base64 string in half.

**`base8` had to be decided rather than looked up**, because no RFC defines base 8 over a byte stream. Three octal digits hold nine bits where a byte has eight, so there is slack: per-byte rendering makes `400` through `777` name no byte, and `base8_decode` rejects them with the arithmetic in the message ("the group at 0 is 256, which is more than a byte holds"). The alternative -- true 3-bit grouping with its own padding rules -- would have invented a scheme nobody else implements, and would have made `base8` a sibling of `base32` rather than of `base2`. Per-byte is the choice; the module header says it is a choice.

**One implementation each, with the old doors kept open.** `crypto`'s four base64 functions and `otp`'s two base32 ones are `#[deprecated]` delegates now, so no caller outside these repositories breaks and `--lint` tells them where the vocabulary went. The base64 C stays registered from `native_crypto.c` beside the other OpenSSL work -- only the declaration moved, to `encoding::base64_encode`. `base16_encode` delegates to `number.to_hex`, which keeps its own name because it also takes an `int`, a `bigint` and a single `byte`: two doors, different jobs, one implementation.

In-tree callers all moved -- 66 call sites across 19 files, including the backend -- because a library calling its own deprecated names would make `--lint` noisy about lines nobody can act on. Nothing in either repository now reaches the deprecated six, which `--lint` on the backend's `main.zm` confirms.

Registering a bundled module takes **three** edits, worth writing down next to the prelude's four: `CORELIB_MODULES` in the Makefile (the header is generated by a pattern rule from `lib/core/<name>.zm`), the `#include` in `corelib.c`, and the `CORE_MODULES` table entry beside it. Miss any and `use encoding` reports "not a bundled core module", which is at least an honest error.

A small thing the move exposed: `otp`'s `val invariant: Locale = locale.root()` had exactly one user, `base32_decode`'s case folding, so it left with it -- along with `use locale`. A module-level `val` is easy to leave behind when its only caller moves out.


## `process.output`'s temporary file was neither unique nor unpredictable (2026-10-24)

`process.zm` needed no rename -- `start`, `wait`, `kill`, `terminate`, `interrupt`, `run`, `output` are already the words. `wait_for` was considered for folding into `wait` by arity and kept apart deliberately: a timeout is a different *outcome*, not a different input, and the `Option<int>` it answers is the shape the name is warning about. What needed fixing was one line.

`output` captures a child's two streams through a temporary file, and built its name as `ziman_process_<pid>_<byte_len(program)>.out`. That is wrong twice.

**It is not unique.** Two concurrent calls whose program names are the *same length* share the path. `/bin/echo` and `/bin/true` are both nine characters, so `output("/bin/echo", ...)` and `output("/bin/true", ...)` from two threads would write one file and each read the other's transcript. Demonstrated rather than reasoned about: the two paths compare equal. This is a library function in a language with threads, so "concurrent" is the ordinary case, not the exotic one.

**It is not unpredictable.** A path determined by a pid and a program-name length can be guessed, and in a shared temporary directory it can be *pre-created as a symbolic link* -- after which the child's output truncates whatever it points at. That is CWE-377, the textbook insecure-temp-file shape, and the fact that both ingredients are guessable is the whole of it.

The name now carries eight bytes from `random.random_bytes`, which reads the operating system's entropy pool, rendered with the `encoding.base16_encode` added earlier the same day. Pre-creating the path means guessing one of 2^64.

**What would be the complete answer, and why it is not available here.** The symlink half is properly closed by opening the file with `O_EXCL`, which fails if the path already exists. That open happens inside `start`, whose `out_path` is a *caller's* path and may legitimately exist already -- a program redirecting into its own log file is the normal use. Demanding `O_EXCL` there would break that, and `output`'s private file has no way to ask for different flags than `start` gives every caller. Randomness is the proportionate fix; the limitation is recorded beside it rather than left for someone to discover.

**And a leak on the error path.** `try run(...)` returned early when the child could not be started, which left the file behind -- one leaked temporary per failed call, and the single route out of this function that did not clean up. It binds the result, removes the file, then propagates. `tests/lang/process_output_temp_file.zm` counts `ziman_process_*` entries in the temporary directory before and after a failing call, so the leak cannot come back silently; it cannot assert the *name* (it is random now, which is the point) so it asserts the properties instead.


## `replace(s, Map<string, string>)`: one pass, longest match (2026-10-24)

A second `replace`, taking a map of what to replace with what. The whole of the design is in one sentence -- **every key replaced by its value in a single pass, longest match first, and a replacement is never rescanned** -- because a *sequential* version would be three lines any caller can already write, and those three lines are exactly where the bugs were.

Four places in `lib/` chained `replace` calls and encoded a constraint in the order:

    replace(replace(replace(text, "\r\n", " "), "\n", " "), "\r", " ")   // markdown.flatten_cell
    replace(replace(s, "\r\n", "\n"), "\r", "\n")                        // xml, normalising endings

Both are correct **only because `\r\n` comes first**. Swap two lines and a CRLF becomes two spaces. With longest match the order cannot matter, which is the difference between code that works and code that works for a reason.

The other two are escapers. `metrics.escape` was three chained calls, correct only because the backslash was done first -- reverse it and `"` becomes `\\"`, the escape escaping the escape it just wrote. `markdown` had the same shape for `\` and `>`. In one pass a replacement is output rather than re-examined, so that bug is not expressible. All four use the map form now, and the suite passed unchanged, which is what makes them conversions rather than rewrites.

`Map { "a": "b", "b": "c" }` leaves `"a"` as `"b"`. An empty key is a runtime error, matching what the three-argument form already does for an empty `old` -- it would match at every position and advance nothing.

**A measurement worth keeping, prompted by a reader's question.** The implementation hoists the keys with `to_array(keys(pairs))`, and the obvious objection is that `for k in pairs` iterates a Map's keys directly and reads better. It does, and it is slower, because the inner loop runs once per *character* and Map iteration has per-loop setup an array walk does not: a 39-character value escaped 200,000 times costs 8779ms that way against 7940ms hoisted, and a 640,000-character document 717ms against 599ms. Building the array by hand with `push` is slower again at 8177ms -- `keys` and `to_array` are natives doing bulk work where a `push` per key is interpreted.

So the hoist stays, with the numbers beside it. The actual defect was that the code gave no reason for it, which is why the question got asked; an optimisation with no comment is indistinguishable from a reflex.


## `keys` answers an array; the `Set` becomes `keys_set` (2026-10-24)

`keys(m)` answered a `Set<K>`, added on 2026-10-22 with the reasoning that a map's keys are unique by construction so the type that says so is the honest one. That reasoning is correct and was the wrong trade. The names are swapped: `keys(m) -> [K]`, `keys_set(m) -> Set<K>`.

**Three reasons, in order of weight.**

`keys` and `values` are *the* canonical pair, and they disagreed about their answer type -- `values` has always answered `[V]`. A reader who learns one learns the wrong thing about the other.

Callers overwhelmingly want to walk the keys. Six places across these two repositories wrote `to_array(keys(m))`. Of the four that used the `Set` directly, **three were `len(keys(m))`** -- which is `len(m)` -- and the fourth a `contains(keys(m), x)` that a `Map` answers itself. So the `Set` form had, in practice, no callers at all; it is a real question, just a rare one.

And the array is the *cheap* one, which the old arrangement had backwards. Building the `Set` hashes every key into a trie that is then walked straight back out into the array that was wanted: 156-183 ms per call against 16-18 ms on a 100,000-entry map.

**The swap is almost entirely source-compatible, by a property worth noticing.** `len` and `contains` both answer for a `Set` and an array, so every `len(keys(m))` and `contains(keys(m), x)` kept working and kept meaning the same thing -- keys are unique either way, and `Set` iteration is insertion-ordered like the map's, so neither duplicates nor order change. The one shape that cannot survive is an explicit `==` against a `Set` literal, `keys(m) == Set<int> {}`, which is precisely the case where the caller meant a set. One fixture had it; it says `keys_set` there now.

**The C side is where this could have gone silently wrong.** Both members are natives, and the symbol the world-prefix fallback derives from `keys` on a `Map` is `map_keys` -- so swapping the declarations without swapping the registrations compiles, passes the whole suite, and answers a `Set` where an array is declared. `map_keys` points at the array implementation now and `map_keys_set` at the set one, with a comment saying why. That is the fifth time this week a native's declaration and its symbol could have come apart invisibly, and the first where *both* candidates were the right arity and type so nothing would have complained.

Found on the way: `tests/lang/keys_array_both_worlds.zm` was named after the function and is `keys_both_worlds.zm`; a blanket rename also collapsed the two names into one inside SYNTAX.md's prose ("`keys(m)`, `keys(m)` and `values(m)`"), which is what a sweep does to a document that discusses both halves of a rename.


## `properties`: two families become one, and both were derive bridges (2026-10-24)

`properties.zm` had `entry_string`/`entry_int`/`entry_float`/`entry_bool` over an `Entry`, and `get_string`/`get_int`/`get_float`/`get_bool` over a `Properties` and a key. Eight names for four questions.

**The `entry_*` four had zero hand-written callers anywhere in either repository**, which looked like dead code and was not: `#[derive(Properties)]` generates `properties.entry_<type>(...)` for an `Option<T>` field and `properties.get_<type>(props, key)` for a required one. Both families are compiler bridges, built by concatenating the field's type onto a prefix -- the same shape as `<type>_to_json` and `<type>_compare`. Checking that before deleting is the whole of the work here; "no callers" and "called by the compiler" look identical from a grep.

**They are one name per type now**, because the `entry_` prefix named the type of the *argument*, which the argument already says: `get_int(e: Entry)` and `get_int(props: Properties, key: string)` differ in both arity and first-parameter type, so every call tells them apart. Derive's two emit sites now write the same name and differ only in what they pass, which is simpler there too.

**The type suffix stays and could not go.** All four take identical arguments and differ only in what they answer -- the constraint that keeps `fs.read_text`/`read_bytes` and `math.floor`/`floor_float` apart, and the sixth time this week it has decided a name. It is also load-bearing for derive, which builds `get_%s` from the field's type: without the suffix the family would not be addressable by generated code at all.

That is the distinction worth carrying away from this week's renames. A derive bridge is not exempt from being renamed -- `time`'s converters moved and `json`'s tree helpers moved -- but it *is* exempt from losing the part of its name the generator computes. `entry_` was a prefix a human chose; `_int` is a suffix the compiler writes.

An honest note on the verification: `make check` reported `api-check` failing on "an enum variant carries its payload type: no 'Io(string)' in the output", and `Io(string)` was plainly in `--api net`'s output at line 69. Running `make api-check` alone passed 41 of 41, and so did the next full run. A stale binary against freshly edited `lib/` sources is the standing explanation for that shape here, and it is the reason the rule is to re-run rather than to reason about a single failure.


## `random`: ten functions over one native, and the bias I overstated (2026-10-24)

`random.zm` had exactly one function, `random_bytes`, which said random twice when qualified. It is `random.bytes` now, qualified-only and deliberately not importable by name -- the stutter is bad as `random.random_bytes(32)`, but a *bare* `random_bytes(32)` was good, where a bare `bytes(32)` says nothing at all. Same rule as `json.format` and `metrics.add`, for a different reason.

Everything added is ordinary Ziman over that one native: `below`, `between`, `fraction`, `boolean`, `choice`, `shuffle`, `text`, `alpha`, `numeric`, `alpha_numeric`.

**`int`, `float`, `bool` and `string` are type keywords, so they cannot be function names.** `below`/`between`/`fraction`/`boolean`/`text` are not stylistic choices. `pub fn int(low: int, high: int)` *declares* and type-checks perfectly well; then every call site is a syntax error, because `random.int(3, 9)` cannot parse -- the parser sees a type where a name must be. Four declarations compiled and four families of call sites failed, which is how it was found. Worth knowing in general: a declaration compiling is no evidence the name is usable.

**Why these exist at all.** The hand-rolled form is `x % n` over random bytes, and it favours the low residues unless `n` divides the draw's range exactly. `bounded_at` rejects the uneven tail -- everything at or above the largest multiple of `span` that fits in 2^56 -- which makes what remains exactly uniform. Seven bytes per draw, because 2^56 is the widest draw from which a rejection limit can be computed without overflowing an `int`. The tail is at most `span - 1` values out of 2^56, so a rejection is rare enough that answering it with a fresh draw costs nothing measurable.

**I overstated the bias, and the measurement is the interesting part.** I said the reference backend had three copies of the pattern, "one of them choosing MFA backup codes", and left the impression all three were wrong. The amount of bias depends entirely on how wide the draw is against the span, so:

* `mfa.zm` drew **4** bytes and took `% 100000000`. 2^32 mod 10^8 is 94967296, so 95% of the eight-digit backup codes were 2.4% more likely than the other 5%. Real, measurable, and in the value that stands in for a second factor.
* `testdata.zm` drew **6** bytes and took `% 3`. 2^48 mod 3 is 1 -- a bias of about 4e-15. Correct, but *by accident* of the draw being sixteen orders of magnitude wider than the span, not by anything the code does.

Rejection sampling removes the dependence on that ratio, and that is the real argument for it: the hand-rolled form is not wrong, it is *conditionally* right, and the condition is invisible at the call site. `mfa.zm` is `random.numeric(8)` now, which also drops its zero-padding loop -- eight digits is what `numeric` answers, not something a caller formats afterwards.

**A `bytes` call is three syscalls** (`fopen`/`fread`/`fclose` on /dev/urandom), so `shuffle` and `text` buy `draw_width * n` bytes in one draw and index into it, rather than calling per value. A rejected draw inside the loop falls back to its own small call.

`choice` answers `Option<T>` rather than erroring on an empty array: an empty array is an ordinary input a caller can hold without having made a mistake, the reading `get` and `lookup` already take. `shuffle` is Fisher-Yates walking down, swapping each position with one at or before it; both naive alternatives are biased and worth naming -- sorting by a random key gives a distribution that depends on the sort's tie handling, and swapping against the *whole* array over-represents some orderings by a factor that grows with the length.

**The three text generators and the three prelude predicates are deliberately the same three questions**, so `is_alpha(alpha(n))`, `is_numeric(numeric(n))` and `is_alpha_numeric(alpha_numeric(n))` are true by construction. That is also what the fixture asserts, because **a randomised function can only be pinned by its properties**: `tests/lang/random_derived.zm` checks bounds coverage over 4000 draws, that `below(n)` never reaches `n`, that `fraction` stays in `[0, 1)`, that `choice` always answers from the pool, that `shuffle` sorts back to its input, and the three predicate agreements -- never a specific value.

Two smaller notes. `out[i] = out[j]` is the way to write into a `MutableArray`; `set(out, i, v)` is not a function and the error says so. And an indented bullet list in a module's header comment comes out of `tools/gen_lib_manual.py` as a fenced code block -- the generator turns any indented run following a blank line into one, which is right for examples and wrong for prose, so doc bullets start at column 0.


## `test`: a failing assertion names the line that asserted (2026-10-24)

Every failing assertion in this repository reported the same line, in a file nobody wrote:

```
FAIL t.zm::eq_labels  runtime error at 18:13: unwrap called on an Err value: assertion failed: left != right
```

18:13 is `lib/core/test.zm`'s own `unwrap`, inside `fail`. It is the first thing a reader sees when a test fails, it is identical for all 1190 tests here and the backend's suite on top, and it names neither the test nor the assertion. **The information was never missing** -- the stack trace has it, and a plain `ziman` run prints it (`at inner (tr.zm:8:5)`). `ziman test` was throwing it away: `run_one_test` reported `boundary.message` and nothing else.

It now reports the line that asserted:

```
FAIL t.zm::eq_labels  at 9:5: assertion failed: expected != actual
  expected: wanted
  actual:   found
```

**Three small pieces, and the design is in which of them knows about `test`.** `ThreadBoundary` gained `raw`, the message without its `runtime error at L:C` prefix -- a caller that reports a failure somewhere other than at that location needs the message apart from it, and that is a general want, not a test-module one. `error.c` gained `ziman_core_entry_site(module, ...)`, which walks down from the innermost frame while the frames belong to a *bundled* module of that name and answers the outermost one's call site -- the line the reader's own code called in at, whose file is the file of the frame below it. Only `interpreter.c`'s `run_one_test` passes `"test"`, so error.c still knows nothing about assertions.

A bundled module's functions are the ones with no `source_file`, so `file == NULL` together with a `module::` name prefix is exact: a reader's own `test.zm` declares a source file and can never be mistaken for this one. The frames are still standing after the longjmp, which is not an assumption -- `thread_failed` already relies on it to print a trace.

**The rewrite is deliberately narrow, and that is the point.** Both conditions must hold -- the innermost frames are the bundled test module's, *and* the message is `unwrap`'s on an `Err`, which is how `fail` raises. An ordinary runtime error inside a test keeps its own location untouched, because that location is already the reader's and losing it would be strictly worse than the noise being removed. The fixture pins both halves: `an_index_out_of_range` and `an_unwrapped_none` still report `runtime error at <their own line>`. An error raised anywhere else inside test.zm falls through unchanged rather than being attributed to a line that did not raise it.

Checked by hand because a fixture is one file: when the assertion is in a helper in *another* file, the message names that file and the line in it.

**The messages contradicted their own parameters.** `assert_eq`'s are `expected` and `actual`; it printed `left` and `right`, so nothing told the reader which was which. They are `expected:`/`actual:` now (`unexpected:`/`actual:` for `assert_ne`), and `assert_lt`/`le`/`gt`/`ge` say `actual {} is not less than bound {}` rather than leaving the roles to be guessed. No call site changed.

**`assert(contains(..))` was the most common assertion in the two repositories -- 109 of them -- and the one that printed the least.** On failure it says `assertion failed` and stops: no haystack, no needle, in exactly the case where the reader is asking which of a 5 KB rendered body's lines is missing. `assert_contains`/`assert_not_contains` cover the shapes the prelude's `contains` takes -- `(string, needle)`, `([T], T)`, `(Set<T>, T)`, `(Map<K, V>, K)` -- and all 109 sites moved. The haystack is shown **in full, not clipped**: a clip hides the thing being looked for whenever the near-miss is past the cut, and `assert_eq` already shows whole values.

`assert_near(expected, actual, tolerance)` is the float answer, because `assert_eq(0.3, 0.1 + 0.2)` fails with `0.30000000000000004` and is right to. Two details worth having checked rather than assumed: a NaN fails however wide the tolerance, *including against itself*, which is the wanted answer since no comparison with a NaN is true -- and a NaN does not come from arithmetic here, because `0.0 / 0.0` is a runtime error in Ziman, not a NaN (`math.sqrt` of a negative and `parse_float("nan")` are where one comes from). A negative tolerance could never pass, so it fails saying that rather than looking like a failed comparison.

One repeat of a known trap, caught in seconds this time: `test` is a *bundled* module, so `--api test` kept listing the old function set until `make ziman` re-embedded the source. Editing a core module's `.zm` and testing without rebuilding tests the binary's copy.


## `test`, continued: what AssertJ was asked for, and the number I got wrong (2026-10-24)

The question was what else `test` should carry, and the answer came from measuring the Spring backend's own tests rather than reading AssertJ's catalog: 90 of its test files use AssertJ, and the Ziman backend reimplements those tests, so what they call is demonstrated demand and not a wish list. `isEqualTo` 639, `isNull`/`isNotNull`/`isPresent` 177, `contains`/`doesNotContain` 116, `isTrue`/`isFalse` 88, `isInstanceOf` 86, `containsEntry` 73, `hasSize`/`isEmpty`/`isNotEmpty` 97, `isCloseTo` 15. The already-covered ones line up with `assert_eq`, the `Option` pair and the `contains` pair; `isCloseTo` landing at 15 is an after-the-fact argument for the `assert_near` added hours earlier.

**Added**: `assert_len`, `assert_empty`/`assert_not_empty` (the four shapes `assert_contains` takes); `assert_all_match`/`assert_any_match`/`assert_none_match`; `assert_entry`; `assert_starts_with`/`assert_ends_with`; `assert_blank`/`assert_not_blank`; `assert_matches`/`assert_not_matches`. 22 functions, taking the module from 22 to 44.

**I reported 85 call sites for the `len` shape and the real number is 57.** The count came from `grep -r ... tests $R/test` with `R=". ../path/src"` unquoted, so word splitting searched `tests`, then `.` -- the whole repository, `tests` a second time -- and the backend. The clean count over `*.zm` in both repositories is 57, of which 15 ask only whether the thing is empty. That also moves the shape from "second most common assertion" to sixth, behind `assert_eq` 340, `assert_ok` 160, `assert_contains` 121, `assert_err` 103 and `assert_some` 51. The argument for the function is unchanged and does not rest on the count -- `assert_eq(3, len(xs))` reports `expected: 3 / actual: 5` and never shows which five, the same defect as `assert(contains(..))` -- but the number in the module's own comment was wrong and is fixed. **An unquoted path variable in a counting grep silently counts the repository root**, which is the lesson worth keeping: a figure that lands in a doc comment deserves the same `--include` discipline as a figure that lands in a decision.

**Three things AssertJ has that Ziman cannot express, said plainly rather than approximated.** `isInstanceOf`, 86 uses there, has no analogue: `$is_variant` is parser-only (`ziman --api builtins` lists it as not callable), so no function can ask "is this value variant X" and the idiom stays `if BadRequest(_) = e`. `assertThatThrownBy`/`catchThrowable` need a catch, which the language does not have; the `Result` form is `assert_err`, already there. `extracting`/`satisfies` are fluent-chain projections, not assertions -- `map(items, fn)` then `assert_eq`. None of the three cost anything today: the Ziman tests check an error *variant* zero times.

**`assert_matches` searches; AssertJ's `matches` does not.** Java's `String.matches` requires the pattern to cover the whole string, while `regex.is_match` -- which this is built on and which a Ziman reader already knows -- matches anywhere. Following the module it sits on beats following the library it was modelled from, so this searches, says so, and points at `^...$` for the other question (checked: the anchors work). A pattern that will not compile fails with the engine's own message (`missing closing parenthesis at position 3`) rather than as a mismatch -- a malformed pattern is a mistake in the test, and reporting it as "did not match" sends the reader to the wrong place entirely.

This is also test.zm's **first dependency**: `use regex`, where every other assertion is prelude-only. Worth naming as a cost, since every `ziman test` run now parses regex's 285 lines whether the test uses it or not.

**The predicate three earn their place by naming the offender.** `assert(all(items, test))` can only answer true or false, leaving the reader to find which element broke the rule; `assert_all_match` and `assert_none_match` report the element and its index. They take `[T]` only, because that is as far as `all`/`any`/`none` themselves reach.

`assert_entry` exists for a sharper reason than a nicer message: `assert_eq(value, unwrap(get(m, key)))` *cannot* report a missing key as an assertion failure, because it dies inside `unwrap` before the assertion runs. Two distinct failures, two distinct messages.

**The mutable worlds are deliberately not covered, and 2 of the 57 migrated sites proved the cost of that.** Both held a `MutableArray`, and both now pass it through `snapshot`. Covering `MutableArray`/`MutableMap`/`MutableSet`/`MutableString` across the size, membership and predicate families is 23 further near-identical functions, and the prelude only repeats itself nine times per function because it is the universal vocabulary with no abstraction to repeat over. test.zm sits a level above that and can lean on `snapshot`. What makes it tolerable is the failure mode: a compile-time type error whose message names the shapes that *are* accepted (`expects string, [T], Set<T> or Map<K, V>, found MutableArray<json.Json>`), so the fix is visible at the place it is needed. If the rate turns out worse than 3.5% the overloads can be added without changing a call site.


## `toml` accepted 30 of 31 documents the spec forbids (2026-10-25)

`toml.zm` says "TOML 1.0" and reads the whole language: every string form, every integer base, dotted keys, inline tables, arrays of tables. Of 31 valid documents thrown at it, it refused exactly one (`1e400`, consistent with its documented `inf`/`nan` stance). **What it did not do was refuse the invalid ones.** Of 31 inputs the spec forbids, it accepted 30 -- the only one it already caught was `a = 1e`, and that by accident of `parse_float` refusing it. Found by writing the invalid half of a conformance suite rather than by reading the code:

* **An array of values accepted `[[table]]` appends.** `a = [1]` followed by `[[a]]` produced `{"a":[1,{}]}` -- a number and a table in one array, which is not a value TOML can express. The spec: "Attempting to append to a statically defined array, even if that array is empty, must produce an error at parse time."
* **Inline tables were not sealed.** All four ways in -- `[a]`, `[a.c]`, `[[a.c]]` and a dotted `a.c = 2` -- extended `a = { b = 1 }` happily. The spec: "Keys and sub-tables cannot be added outside the braces."
* **A dotted key's table could be redefined by a header.** `a.b = 1` then `[a]`.
* **Underscores needed no digits around them.** `1__0`, `10_` and `0x_1` all parsed, because `replace(tok, "_", "")` runs before anything looks at where they were -- stripping them is precisely what destroys the evidence.
* **`1.` and `1.e5` were floats**, read as 1.0 and 100000.0, because `parse_float` is laxer than TOML's grammar. `01.5` too: the no-leading-zeros rule was checked for integers only.
* **Dates and times were never range-checked.** `1979-02-30`, `2023-02-29`, month 00, month 13, day 32, `25:00:00`, `23:60:00`, `23:59:61`, an offset of `+99:00`, and `07:32` with no seconds at all. The scanner matched a *shape*, and a shape holds any digits.
* **Raw control characters sat in strings** -- U+0007 and U+007F, in basic, literal and multi-line alike. Nothing was corrupted (DEL came back as bytes `[120, 127, 121]`, checked), it was simply let through.

**The structural five needed something the tree cannot hold.** A `Json` object is a table whether it came from `[a]` or from `a = { .. }`; a `Json` array is an array of tables whether it came from `[[a]]` or from `a = [1]`. The spec treats those origins very differently, so the origin has to be remembered beside the tree: three sets of dotted paths (`inline_tables`, `static_arrays`, `dotted_tables`) next to the `defined` set that was already there.

**`dotted_tables` is separate from `defined` for a reason the spec is precise about.** A table the parser created implicitly from a *header* path may be declared later -- `[x.y.z.w]` then `[x]` is explicitly fine. One created by a *dotted key* may not. And only the exact tables, not everything beneath them: the spec's worked example makes `[fruit.apple]` and `[fruit.apple.taste]` invalid after `apple.color` and `apple.taste.sweet`, while `[fruit.apple.texture]` stays legal. All three verdicts are now a test, because the rule is the kind that is easy to implement one notch too strictly.

**The case that forced the sets to have a scope**: a fresh `[[p]]` element is a fresh table, so `a` having been an inline table in the previous element says nothing about this one. `[[p]] / a = { x = 1 } / [[p]] / a.y = 2` is legal TOML, and a flag rather than a scope would refuse it. `forget_under` drops every path beneath the header's on each `[[p]]`, keeping its dot so `[[p]]` forgets `p.a` and leaves `pq.a` alone.

**On the dates: the decision was to delegate the calendar, and measuring changed how.** `time.parse_localdate` answers the whole question and says which component was wrong ("invalid day: 30 (month 2 of 1979 has 28 days)"), and that is how this was first written. It is a format-driven parse, and at about 18 microseconds per date a document of 2000 of them felt it. What the delegation was *for* is the leap-year rule -- divisible by 4, except centuries, except every fourth century -- which is the one piece of real calendar knowledge involved. So `time.is_leap_year` stays, and a table of twelve month lengths sits beside it: a table is not knowledge. The dependency the decision bought is kept; the per-date parse is not.

**TOML and `time` disagree on exactly one value, and TOML wins here.** RFC 3339 defines `time-second` as "00-58, 00-59, 00-60 based on leap second rules" and toml.io is silent, so `23:59:60` is a value a TOML document may hold -- while `time.parse_time` refuses it outright. That is why the clock half is three range checks in `toml.zm` rather than a second delegation. Second 61 is still an error.

**Validating costs about a fifth of the parse, and the cost is not where it looks.** Measured best-of-7 on a 203 KB document: 130 ms to 162 ms with no dates, 194 ms to 237 ms with 2000 of them. The dates are about 5 microseconds each now; the bulk is the control-character test, which every character of every string must pass -- `parse_basic` went from 5.9% of the profile to 7.7%, with `is_control` another 3.3%. There is no cheaper way to know a string holds no raw control character than to look at each one.

**Two measurement mistakes worth recording, because both would have produced a wrong conclusion.** The first draft of `is_control` read `to_bytes(c)[0]`, which allocates a byte array *per character of every string*; it put 23% of the run in the collector and cost most of an initial 30% slowdown. Written as `compare(c, " ") < 0` -- which `quote_basic` next door already did -- it allocates nothing, and string order is byte order so no multi-byte character can sort below a space. The second: I first attributed the remaining cost to date validation by comparing a document of bare dates against one of quoted dates. Those are not the same measurement -- a quoted date goes through `parse_basic` and a bare one through the number scanner -- and the honest comparison is one binary against the other on the *same* file. Single-run timings on this machine vary by 20%, which is wide enough to support whichever story one is looking for; best-of-7 in-process, alternating the two binaries, is what settled it.

Left as it was: the module stays `#![experimental]`, and dates remain `JString`. That second one deserves saying out loud rather than only in a parenthesis, so the header now does: a document that goes through `parse` and back through `format` comes out with its dates **quoted, as strings**. Only the tree round-trips, not the document.


## `uuid`: the rename that should not happen, and a v7 that was not ordered (2026-10-25)

`uuid.zm` looked like the next `random.random_bytes`. It has `uuid_new`, `uuid_new_v7` and `uuid_nil`, which qualified read `uuid.uuid_new()` -- the same stutter, in the same shape, one module over. **The call sites said no, by 303 to 6.**

The module is used through *by-name imports*: `use uuid.uuid_new` then `uuid_new()`, `use uuid.uuid_to_sql` then `uuid_to_sql(patient_id)`. 303 bare call sites against 6 qualified ones. A bare `new()` or `to_sql(id)` says nothing about what it answers, so the prefix is the whole content of the name there.

**It is the same rule as `random.bytes`, giving the opposite answer, and that is the thing worth keeping.** The question is not "does the name repeat the module" but "does the *call site* say what the function answers". `random_bytes(32)` read well bare and `bytes(32)` says nothing, so that one became `random.bytes` and qualified-only. `uuid_new()` reads well bare and `new()` says nothing, so this one keeps its prefix. Two opposite conclusions from one rule, and the way to tell them apart is to count how the code actually calls them rather than to look at the declaration.

The module already holds both spellings, consistently, and the counts prove it was deliberate: `format`, `parse` and `version` are bare and reached as `uuid.format(u)` (71, 26 and 3 qualified calls), while the two by-name imports of them are *aliased* -- `use uuid.parse as parse_id`, `use uuid.format as show` -- because a bare `parse` collides with json's and toml's. So the new functions follow the split: `uuid_of` and `uuid_max` take the prefix, `is_nil` and `timestamp_millis` do not.

`uuid_to_json` and friends were never candidates: derive.c's `standard_flat_type_name` already records why (`Uuid` keeps `uuid_to_json` "because its name has nothing to shorten"), since the type name *is* the module name.

**The real bug: version 7 was not time-ordered.** Its whole purpose is that UUIDs made later sort after earlier ones, which is why it makes a good database key, and the module said exactly that. Measured over 2000 made in a row: **974 of 1999 consecutive pairs sorted backwards.** With milliseconds in the first 48 bits and random in the rest, everything made inside one millisecond is in random order, and a tight loop stays inside one millisecond.

RFC 9562 offers the fix and names it -- "replace leftmost random bits with increased clock precision". `rand_a`, the 12 bits after the version nibble, now carries the fraction of the millisecond rather than random: 4096 steps, one per 244 nanoseconds. `clock.now_raw` has genuine nanosecond resolution (consecutive reads differ by a few hundred), so two calls in a row land in different steps. 974 out of order became **0 out of 2999**, and the fixture now does the tight loop the old one carefully avoided -- it slept 3 ms between its two v7s, which is precisely the case that worked.

Stated rather than hidden: ordering follows `CLOCK_REALTIME`, so a clock stepped backwards produces UUIDs that sort before ones already made. That is inherent to v7 -- the timestamp is wall-clock by definition -- and the alternative, a monotonic counter, needs state shared between threads. The 12 bits also stop being random, leaving 62, which is what RFC 9562 intends.

**The header cited RFC 4122, which does not contain version 7.** 9562 obsoleted it. The variant bits really are 4122's, so that constant keeps its name and the header now says which is which.

**`Uuid { bytes: [1b, 2b] }` was constructible, and the consequences landed inside the module.** Ziman has no per-field privacy, so nothing can stop it; what was missing was a sanctioned way in. `uuid_of(bytes)` answers an `Err` naming the length, at the line that made the mistake, instead of `format` dying three frames deeper on `substring index out of bounds`.

`uuid_compare` had the sharper version of that problem. It walked `0..len(a.bytes)` and indexed `b.bytes[i]`, so comparing a short UUID against a full one read off the end -- **but only when the leading bytes happened to match**, which they usually do not. A crash that depends on the values is worse than one that does not, because it survives every test that does not happen to hit it. It compares the common prefix now and uses the length as the last tiebreak.

**The entropy cost, measured and then deliberately left alone.** `uuid_new` is about 9.4 microseconds, of which 7.2 are `random.bytes` -- three syscalls for 16 bytes, where a single draw of 80,000 bytes measures under a millisecond. Buffering a block and slicing it would be roughly four times faster, and would need state shared between every caller: a program making UUIDs from a `spawn` would race on it. One UUID per request costs nothing, and a loop making a million should draw its own bytes and call `uuid_of`. That is now in the header, so the next reader finds the measurement and the reason rather than re-deriving both.

Added while there: `uuid_max` (the all-ones UUID RFC 9562 defines beside Nil, which sorts after everything), `is_nil` (reads the bytes rather than building a UUID to compare against), and `timestamp_millis`, which reads the 48 bits back out and answers `None` for any other version. It answers a plain number, not a `time.Instant`, so the module gains no dependency on `time` -- `instant_of(millis / 1000, millis % 1000 * 1000000)` is the bridge, and it is in the comment. The 12 bits of finer time are deliberately left out of it: they order, they do not measure.
