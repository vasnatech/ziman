# The CPython `dict` side of tests/bench/map_ops.zm -- same container size, same operation count,
# same loop-overhead subtraction, so the two tables are directly comparable. See that file.
#
# Every measured loop is the baseline loop plus exactly the operation -- same `k = i % SIZE`, same
# accumulate -- because a first version did not do that for `len` and the store rows and reported
# `len` at -5 ns/op and `[k] = v` at 1 ns/op.
import time

SIZE = 100_000
OPS = 1_000_000


def report(label, secs, base):
    # OPS == 1,000,000, so elapsed seconds * 1000 is ns/op.
    print(f"  {label}  {round((secs - base) * 1000)} ns/op")


def main():
    d = {i: i for i in range(SIZE)}
    print(f"dict over {SIZE} entries, {OPS} ops each, loop overhead subtracted")

    acc = 0
    t = time.perf_counter()
    for i in range(OPS):
        k = i % SIZE
        acc += k
    base = time.perf_counter() - t
    print(f"  (loop overhead {round(base * 1000)} ns/op, subtracted below)")

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
    report("dict  in             ", time.perf_counter() - t, base)

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


main()
