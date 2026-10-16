// The java.util.HashMap side of tests/bench/map_ops.zm and map_ops.py. Run with
// `java tests/bench/map_ops.java` (single-file source, no build step).
//
// READ THIS BEFORE QUOTING THE PER-ROW NUMBERS. They are indicative only. The per-row method the
// other two files use -- time a loop, time a baseline loop, subtract -- does not survive a JIT,
// and two attempts here failed in opposite directions:
//
//   1. Plain loops. `size` reported **-2 ns/op** and `get` 1 ns/op. `d.size()` is loop-invariant
//      so it was hoisted out entirely, and the baseline `acc += i % SIZE` vectorises while a loop
//      containing a map call cannot -- so the baseline was optimised harder than the measurement
//      and the subtraction reported nonsense.
//   2. A volatile sink in both loops, to block vectorisation symmetrically. That removed the
//      negatives, but a volatile store on x86 carries a store-load fence costing more than the
//      operations themselves, so it dominated and compressed every difference: `containsKey` came
//      out at 1 ns/op, still impossible. One bias traded for another.
//
// The sink is still here because it is the lesser of the two, and the `size` row is gone because
// it cannot be measured this way at all. Treat the rows as "single digits to low teens of
// nanoseconds" and no finer. **JMH is the right tool** -- it consumes results through a Blackhole
// rather than a fence, and controls compilation, OSR and dead-code elimination. It was not
// available offline when this was written.
//
// THE COMPOSITE ROW AT THE END IS THE ONE TO QUOTE. It times a single read-modify-write workload
// with no baseline and no subtraction, so there is nothing for the JIT to optimise asymmetrically,
// and map_ops.zm and map_ops.py both report the same row. Measured 2026-10-15 on one machine:
// Java 16-19 ns/op, CPython 3.14 94-98, Ziman 124-129.
//
// Two differences that are part of the comparison rather than noise in it.
//
// Boxing. A Ziman `int` lives inline in a Value and is never boxed. CPython allocates a PyLong for
// `i % SIZE` outside its small-int cache. Java boxes to Integer outside the -128..127 cache, on
// every get and put. HashMap<Integer,Integer> is used because it is what Java code actually
// writes; a primitive-specialised map would be measuring a different language.
//
// And `remove`, where Java differs from Ziman most interestingly: it hands back the value it
// removed, like `mutable_map_remove`, but returns the existing reference rather than wrapping it,
// so it pays none of the Option allocation that made Ziman add `mutable_map_discard`. Java gets
// remove-with-the-value for the price of discard.
import java.util.HashMap;
import java.util.Map;

public class map_ops {
    static final int SIZE = 100_000;
    static final int OPS = 5_000_000;
    static long acc = 0;
    /* A volatile sink, written once per iteration by *every* loop below including the two
     * baselines. It is not decoration: without it the JIT vectorises `acc += i % SIZE` in the
     * baseline and cannot vectorise a loop containing a map call, so the baseline gets optimised
     * harder than the thing being measured and the subtraction reports nonsense -- the first
     * version of this file reported `size` at -2 ns/op and `get` at 1. A volatile store blocks
     * that reordering in both, and costs the same in both, so it subtracts out. */
    static volatile int sink = 0;

    static void report(String label, long nanos, long base) {
        System.out.printf("  %s  %d ns/op%n", label, Math.round((nanos - base) / (double) OPS));
    }

    static void measure(boolean print) {
        Map<Integer, Integer> d = new HashMap<>();
        for (int i = 0; i < SIZE; i++) d.put(i, i);
        if (print) {
            System.out.printf("HashMap over %d entries, %d ops each, loop overhead subtracted%n", SIZE, OPS);
            System.out.println("  (per-row figures are INDICATIVE ONLY on a JIT -- see this file's header;");
            System.out.println("   the composite row at the end is the one to compare across languages)");
        }

        long t = System.nanoTime();
        for (int i = 0; i < OPS; i++) {
            int k = i % SIZE;
            sink = k;
            acc += k;
        }
        long base = System.nanoTime() - t;

        t = System.nanoTime();
        for (int i = 0; i < OPS; i++) {
            int k = i % SIZE;
            sink = k;
            acc += k;
            if (k >= 0) acc += 1;
        }
        long baseIf = System.nanoTime() - t;
        if (print) {
            System.out.printf("  (loop overhead %d ns/op, value-returning rows)%n",
                    Math.round(base / (double) OPS));
            System.out.printf("  (if-shaped overhead %d ns/op, bool-returning rows)%n",
                    Math.round(baseIf / (double) OPS));
        }

        t = System.nanoTime();
        for (int i = 0; i < OPS; i++) {
            int k = i % SIZE;
            sink = k;
            acc += k + d.get(k);
        }
        if (print) report("HashMap  get(k)            ", System.nanoTime() - t, base);

        t = System.nanoTime();
        for (int i = 0; i < OPS; i++) {
            int k = i % SIZE;
            sink = k;
            acc += k + d.getOrDefault(k, 0);
        }
        if (print) report("HashMap  getOrDefault      ", System.nanoTime() - t, base);

        t = System.nanoTime();
        for (int i = 0; i < OPS; i++) {
            int k = i % SIZE;
            sink = k;
            acc += k;
            if (d.containsKey(k)) acc += 1;
        }
        if (print) report("HashMap  containsKey       ", System.nanoTime() - t, baseIf);

        /* No `size` row. Nothing in such a loop modifies the map, so `d.size()` is loop-invariant
         * and the JIT hoists it: the row measures an addition, and reported -2 ns/op when it was
         * here. Ziman and CPython are tree-walker and bytecode VM respectively and re-read it every
         * iteration, so the three are not measuring the same thing anyway. */

        t = System.nanoTime();
        for (int i = 0; i < OPS; i++) {
            int k = i % SIZE;
            sink = k;
            acc += k;
            d.put(k, i);
        }
        if (print) report("HashMap  put               ", System.nanoTime() - t, base);

        t = System.nanoTime();
        for (int i = 0; i < OPS; i++) {
            int k = i % SIZE;
            sink = k;
            acc += k;
            d.remove(k);
            d.put(k, k);
        }
        if (print) report("HashMap  remove + put      ", System.nanoTime() - t, base);

        if (print) System.out.printf("  (checksum %d, sink %d, entries %d)%n", acc, sink, d.size());

        /* The composite: one read-modify-write workload, timed whole. No baseline and nothing
         * subtracted, so there is no second loop for the JIT to optimise differently -- which is
         * the whole reason this row is trustworthy where the ones above are not. map_ops.zm and
         * map_ops.py time the identical workload and print the identical checksum. */
        Map<Integer, Integer> c = new HashMap<>();
        for (int i = 0; i < SIZE; i++) c.put(i, 0);
        long t2 = System.nanoTime();
        for (int i = 0; i < OPS; i++) {
            int k = i % SIZE;
            c.put(k, c.getOrDefault(k, 0) + 1);
        }
        long ns = System.nanoTime() - t2;
        if (print) {
            System.out.printf("%nCOMPOSITE read-modify-write, no subtraction: %.1f ns/op  (checksum %d)%n",
                    ns / (double) OPS, c.get(7));
        }
    }

    public static void main(String[] args) {
        // Two untimed passes first: these figures are meaningless before the JIT has compiled the
        // loops, which is the one way this benchmark differs from its Ziman and CPython twins.
        measure(false);
        measure(false);
        measure(true);
    }
}
