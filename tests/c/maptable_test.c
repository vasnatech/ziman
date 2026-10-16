/* Standalone correctness test for src/maptable.c, the flat table behind MutableMap/MutableSet.
 * Lives outside src/ (which the Makefile wildcards into the ziman binary) since it has its own
 * main(). Run via `make test-maptable`.
 *
 * `value_hash` and `value_key_equals` are declared in value.h and defined in interpreter.c, which
 * would drag in the whole interpreter; this file defines its own instead. That is also what lets
 * the collision tests exist at all: `bad_hash` below returns the same hash for every key, so every
 * insert collides and the probe sequence, the tombstone reuse and the rehash all get exercised on
 * purpose rather than by luck. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/maptable.h"

static int failures = 0;

static void check(const char *label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

/* Stand-ins for interpreter.c's. `collide` makes every key hash alike when set. */
static int collide = 0;
static const Type g_int_type = {.kind = TY_INT};

uint64_t value_hash(Value v) {
    if (collide) return 42;
    uint64_t h = (uint64_t)v.as.i;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

int value_key_equals(Value a, Value b) {
    return a.as.i == b.as.i;
}

static Value key_of(int64_t n) {
    Value v;
    v.type = &g_int_type;
    v.as.i = n;
    return v;
}

typedef struct {
    int64_t seen[4096];
    size_t count;
} Order;

static void record(Value key, Value val, void *ctx) {
    (void)val;
    Order *o = (Order *)ctx;
    if (o->count < 4096) o->seen[o->count++] = key.as.i;
}

static void run_suite(const char *prefix, int with_collisions) {
    char label[160];
    collide = with_collisions;
    MapTable t;
    maptable_init(&t);

    snprintf(label, sizeof label, "%s: a fresh table is empty", prefix);
    check(label, maptable_len(&t) == 0);

    Value out;
    snprintf(label, sizeof label, "%s: get on an empty table finds nothing", prefix);
    check(label, maptable_get(&t, key_of(1), &out) == 0);

    snprintf(label, sizeof label, "%s: the first insert reports the key as new", prefix);
    check(label, maptable_set(&t, key_of(1), key_of(100)) == 1);

    snprintf(label, sizeof label, "%s: and is then found with its value", prefix);
    check(label, maptable_get(&t, key_of(1), &out) == 1 && out.as.i == 100);

    snprintf(label, sizeof label, "%s: re-setting a key overwrites and reports not-new", prefix);
    check(label, maptable_set(&t, key_of(1), key_of(101)) == 0);
    snprintf(label, sizeof label, "%s: the overwrite is visible and the count is unchanged", prefix);
    check(label, maptable_get(&t, key_of(1), &out) == 1 && out.as.i == 101 && maptable_len(&t) == 1);

    /* Enough entries to force several rehashes. */
    for (int64_t i = 2; i <= 500; i++) maptable_set(&t, key_of(i), key_of(i * 2));
    snprintf(label, sizeof label, "%s: 500 entries all present after rehashing", prefix);
    int all_found = 1;
    for (int64_t i = 2; i <= 500; i++) {
        if (!maptable_get(&t, key_of(i), &out) || out.as.i != i * 2) all_found = 0;
    }
    check(label, all_found && maptable_len(&t) == 500);

    snprintf(label, sizeof label, "%s: a key never inserted is not found among 500", prefix);
    check(label, maptable_get(&t, key_of(99999), &out) == 0);

    /* Removal: reported once, gone afterward, and the count drops. */
    snprintf(label, sizeof label, "%s: remove reports the value it removed", prefix);
    check(label, maptable_remove(&t, key_of(250), &out) == 1 && out.as.i == 500);
    snprintf(label, sizeof label, "%s: removing twice reports nothing the second time", prefix);
    check(label, maptable_remove(&t, key_of(250), &out) == 0 && maptable_len(&t) == 499);
    snprintf(label, sizeof label, "%s: a removed key is not found", prefix);
    check(label, maptable_get(&t, key_of(250), &out) == 0);
    snprintf(label, sizeof label, "%s: its neighbours survive the removal", prefix);
    check(label, maptable_get(&t, key_of(249), &out) == 1 && out.as.i == 498 &&
                     maptable_get(&t, key_of(251), &out) == 1 && out.as.i == 502);

    /* Re-inserting a removed key must take a tombstone slot and be findable again. */
    snprintf(label, sizeof label, "%s: a removed key can be re-inserted as new", prefix);
    check(label, maptable_set(&t, key_of(250), key_of(7)) == 1);
    snprintf(label, sizeof label, "%s: and reads back the new value", prefix);
    check(label, maptable_get(&t, key_of(250), &out) == 1 && out.as.i == 7 && maptable_len(&t) == 500);

    maptable_release(&t);

    /* Insertion order, including across a removal and a re-insert: a re-inserted key goes to the
     * end, which is what `dict` does and what the trie's sequence numbers did. */
    maptable_init(&t);
    for (int64_t i = 0; i < 6; i++) maptable_set(&t, key_of(i), key_of(i));
    maptable_remove(&t, key_of(2), NULL);
    maptable_set(&t, key_of(2), key_of(2));
    Order o = {{0}, 0};
    maptable_visit(&t, record, &o);
    int64_t want[] = {0, 1, 3, 4, 5, 2};
    int order_ok = o.count == 6;
    for (size_t i = 0; order_ok && i < 6; i++) {
        if (o.seen[i] != want[i]) order_ok = 0;
    }
    snprintf(label, sizeof label, "%s: visit yields insertion order, re-inserts going last", prefix);
    check(label, order_ok);

    /* A removal-heavy run: the dead entries have to be compacted away by a rehash rather than
     * growing `entries` without bound. */
    for (int64_t i = 100; i < 1100; i++) {
        maptable_set(&t, key_of(i), key_of(i));
        maptable_remove(&t, key_of(i), NULL);
    }
    snprintf(label, sizeof label, "%s: 1000 insert/remove pairs leave the count unchanged", prefix);
    check(label, maptable_len(&t) == 6);
    snprintf(label, sizeof label, "%s: and do not grow the entries array without bound", prefix);
    check(label, t.entry_cap <= 256);

    maptable_clear(&t);
    snprintf(label, sizeof label, "%s: clear empties the table", prefix);
    check(label, maptable_len(&t) == 0 && maptable_get(&t, key_of(0), &out) == 0);
    snprintf(label, sizeof label, "%s: and the table still works afterward", prefix);
    check(label, maptable_set(&t, key_of(5), key_of(5)) == 1 && maptable_len(&t) == 1);

    maptable_release(&t);
    snprintf(label, sizeof label, "%s: release leaves a reusable, empty table", prefix);
    check(label, maptable_len(&t) == 0 && t.indices == NULL && t.entries == NULL);
    maptable_release(&t); /* idempotent */
}

int main(void) {
    run_suite("distinct hashes", 0);
    run_suite("every key colliding", 1);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
