# The CPython `dict` side of tests/bench/map_ops.zm -- same container size, same operation count,
# same loop-overhead subtraction, so the two tables are directly comparable. See that file -- in
# particular its note on why int keys are CPython's best case (hash(i) == i, slot is hash & mask,
# no collisions, no hashing cost) and why the string-key section below is the fairer half of the
# comparison, with the caveat that a CPython `str` caches its hash on the object while a Ziman
# string cannot.
#
# Every measured loop is a baseline loop plus exactly the operation, and there are TWO baselines,
# one per loop shape: a bool-returning operation has to be branched on, and the branch is not free.
# See map_ops.zm's header for the three separate ways this subtraction has been got wrong.
import time

SIZE = 100_000
OPS = 5_000_000


def report(label, secs, base):
    # ns/op is (secs * 1e9) / OPS; matches map_ops.zm's own ops/report pairing.
    print(f"  {label}  {round((secs - base) * 1e9 / OPS)} ns/op")


def main():
    d = {i: i for i in range(SIZE)}
    print(f"dict over {SIZE} entries, {OPS} ops each, loop overhead subtracted")

    acc = 0
    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
    base = time.perf_counter() - t
    print(f"  (loop overhead {round(base * 1e9 / OPS)} ns/op, value-returning rows)")

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
        if k >= 0:
            acc += 1
    base_if = time.perf_counter() - t
    print(f"  (if-shaped overhead {round(base_if * 1e9 / OPS)} ns/op, bool-returning rows)")

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k + d[k]
    report("dict  [k]            ", time.perf_counter() - t, base)

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k + d.get(k, 0)
    report("dict  get(k, default)", time.perf_counter() - t, base)

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
        if k in d:
            acc += 1
    report("dict  in             ", time.perf_counter() - t, base_if)

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k + len(d)
    report("dict  len            ", time.perf_counter() - t, base)

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
        d[k] = i
    report("dict  [k] = v        ", time.perf_counter() - t, base)

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
        del d[k]
        d[k] = k
    report("dict  del + set      ", time.perf_counter() - t, base)

    print(f"  (checksum {acc}, entries {len(d)})")

    # ---- the same questions with string keys ----
    keys = [f"key-{i}" for i in range(SIZE)]
    sd = {keys[i]: i for i in range(SIZE)}
    print(f"\nSame over {SIZE} string keys (pre-built, so no key construction is measured)")

    # The baseline indexes the key list and reads the key's length, as each row below does.
    t = time.perf_counter()
    for i in range(OPS):
        j = i % SIZE
        k = keys[j]
        acc += len(k)
    sbase = time.perf_counter() - t
    print(f"  (loop overhead {round(sbase * 1e9 / OPS)} ns/op, value-returning rows)")

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
        if k >= 0:
            acc += 1
    sbase_if = time.perf_counter() - t
    print(f"  (if-shaped overhead {round(sbase_if * 1e9 / OPS)} ns/op, bool-returning rows)")

    t = time.perf_counter()
    for i in range(OPS):
        j = i % SIZE
        k = keys[j]
        acc += len(k) + sd.get(k, 0)
    report("dict  get(k, default)", time.perf_counter() - t, sbase)

    t = time.perf_counter()
    for i in range(OPS):
        j = i % SIZE
        k = keys[j]
        acc += len(k)
        if k in sd:
            acc += 1
    report("dict  in             ", time.perf_counter() - t, sbase_if)

    t = time.perf_counter()
    for i in range(OPS):
        j = i % SIZE
        k = keys[j]
        acc += len(k)
        sd[k] = j
    report("dict  [k] = v        ", time.perf_counter() - t, sbase)

    print(f"  (checksum {acc}, entries {len(sd)})")

    # ---- set, matching map_ops.zm's Set/MutableSet section ----
    ss = set(range(SIZE))
    print(f"\nset over {SIZE} items, {OPS} ops each")

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
    setbase = time.perf_counter() - t
    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
        if k >= 0:
            acc += 1
    setbase_if = time.perf_counter() - t
    print(f"  (loop overhead {round(setbase * 1e9 / OPS)} ns/op value-returning,"
          f" {round(setbase_if * 1e9 / OPS)} ns/op if-shaped)")

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
        if k in ss:
            acc += 1
    report("set  in              ", time.perf_counter() - t, setbase_if)

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k + len(ss)
    report("set  len             ", time.perf_counter() - t, setbase)

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
        ss.add(k)
    report("set  add (present)   ", time.perf_counter() - t, setbase)

    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
        ss.discard(k)
        ss.add(k)
    report("set  discard + add   ", time.perf_counter() - t, setbase)

    print(f"  (checksum {acc}, items {len(ss)})")

    # ---- the composite, the one row that compares across languages ----
    # One read-modify-write workload timed whole: no baseline, nothing subtracted. See
    # map_ops.java's header for why the per-row method cannot be trusted on a JIT, and map_ops.zm
    # for this row's rationale. All three print the same checksum.
    c = {i: 0 for i in range(SIZE)}
    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        c[k] = c.get(k, 0) + 1
    ns = (time.perf_counter() - t) * 1e9 / OPS
    print(f"\nCOMPOSITE read-modify-write, no subtraction: {ns:.1f} ns/op  (checksum {c[7]})")


main()
