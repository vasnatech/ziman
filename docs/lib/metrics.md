# `metrics`

Counters, gauges and histograms that the threads of a server share, and a render of them in the
Prometheus text format for something to scrape. The numbers live in a native registry
(src/native_metrics.c) because state shared across threads has to sit behind a native handle the
way `MutableString` and `Iter` do. Corrected 2026-10-10 (F40): this used to say the reason was
that a Ziman lambda cannot mutate what it captures, which overstates it -- a lambda *can* mutate
through a captured builder handle, and a memoising closure works today. The binding reason is
narrower and stronger: a builder cannot cross a `spawn` at all (it belongs to the thread that
made it), so anything every request thread must share needs native atomics regardless.

Declare at start-up, record anywhere:


```
metrics.counter("requests_total", "Requests answered, by method and route.")
metrics.histogram("request_seconds", "How long a request took.", [0.005, 0.05, 0.5, 5.0])
...
metrics.increment_with("requests_total", Map<string, string> { "method": "GET" })
metrics.observe("request_seconds", 0.0121)
print(metrics.render())
```

Recording never fails and never reports: a counter increment that could fail would mean a
request failing because it was being measured. Two things follow, and both are deliberate.
Recording to a name that was never declared does nothing -- a typo is silent, so declare in one
place and reference that. And a metric accepts at most 128 distinct label sets, after which
writes are dropped and counted in `ziman_metrics_dropped_series_total`, which `render` always
emits, even at zero. That cap is the whole reason to label with a route *pattern* and never a
raw path: a label carrying a user id or an id-bearing URL grows without bound, which is the
usual way a metrics integration takes a service down rather than measuring it.

Labels are a `Map<string, string>`. The key a series is stored under is built here, with the
names sorted, so the same labels given in a different order are the same series.

`use metrics` -- or `use metrics.Name` to bring one item in bare.

## Example

```
use metrics

// Counters, gauges and histograms in the Prometheus text format, so a scrape endpoint is a few
// lines rather than a dependency. A metric is registered once with the help text that explains it,
// and incremented from anywhere after that -- the registry is the process's, not a value to thread
// through every call.
fn demo() -> Result<unit, string> {
    // Registration answers false only when the registry is full (64 names) -- a start-up mistake
    // rather than something to handle per call, which is why it is a bool and not a Result.
    print(metrics.counter("requests_total", "Requests served"))
    metrics.gauge("queue_depth", "Items waiting")
    metrics.histogram("request_seconds", "Request duration", [0.1, 0.5, 1.0])

    metrics.increment("requests_total")
    metrics.increment("requests_total")
    metrics.add("requests_total", 3.0)

    metrics.set("queue_depth", 7.0)

    metrics.observe("request_seconds", 0.05)
    metrics.observe("request_seconds", 0.3)
    metrics.observe("request_seconds", 2.0)

    print(metrics.render())
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("metrics example failed: {}\n", e)
    }
}
```

Output:

```
true
# HELP requests_total Requests served
# TYPE requests_total counter
requests_total 5
# HELP queue_depth Items waiting
# TYPE queue_depth gauge
queue_depth 7
# HELP request_seconds Request duration
# TYPE request_seconds histogram
request_seconds_bucket{le="0.1"} 1
request_seconds_bucket{le="0.5"} 2
request_seconds_bucket{le="1"} 2
request_seconds_bucket{le="+Inf"} 3
request_seconds_sum 2.35
request_seconds_count 3
# HELP ziman_metrics_dropped_series_total Writes dropped because a metric was at its series cap.
# TYPE ziman_metrics_dropped_series_total counter
ziman_metrics_dropped_series_total 0
```

### `counter`

```
pub fn counter(name: string, help: string) -> bool
```

Declares a counter: a number that only goes up (requests, errors, bytes). False when the
registry is full (64 names), which is a start-up mistake rather than something to handle.

### `gauge`

```
pub fn gauge(name: string, help: string) -> bool
```

Declares a gauge: a number that goes up and down (open connections, queue depth).

### `histogram`

```
pub fn histogram(name: string, help: string, buckets: [float]) -> bool
```

Declares a histogram with fixed upper bounds, in the unit being observed and in increasing
order (seconds, not milliseconds, by Prometheus convention). At most 16; `+Inf` is added by the
render and must not be given here.

### `increment`

```
pub fn increment(name: string)
```

Adds 1 to a counter with no labels.

```
pub fn increment(name: string, labels: Map<string, string>)
```

The same with labels.

```
pub fn increment(s: Series)
```

The same to a resolved series: no strings, no sorting, no lookup by name.

### `add`

```
pub fn add(name: string, delta: float)
```

Adds `delta` to a counter with no labels.

```
pub fn add(name: string, delta: float, labels: Map<string, string>)
```

The same with labels.

```
pub fn add(s: Series, delta: float)
```

The same to a resolved series.

### `set`

```
pub fn set(name: string, value: float)
```

Sets a gauge with no labels.

```
pub fn set(name: string, value: float, labels: Map<string, string>)
```

The same with labels.

```
pub fn set(s: Series, value: float)
```

The same to a resolved series.

### `gauge_add`

```
pub fn gauge_add(name: string, delta: float)
```

Moves a gauge by `delta`, which `add` deliberately will not do: `add` is for counters, and a
counter and a gauge are different promises. Two threads moving the same gauge at once each see the
other's change, which is what an open-connections count needs.

```
pub fn gauge_add(name: string, delta: float, labels: Map<string, string>)
```

The same with labels.

```
pub fn gauge_add(s: Series, delta: float)
```

The same to a resolved series.

### `observe`

```
pub fn observe(name: string, value: float)
```

Records one observation in a histogram with no labels.

```
pub fn observe(name: string, value: float, labels: Map<string, string>)
```

The same with labels.

```
pub fn observe(s: Series, value: float)
```

The same to a resolved series.

### `Series`

```
pub struct Series {
    handle: int
}
```

One metric's one label set, resolved once so that recording to it costs nothing but the update.

Building a label key is the expensive half of a labelled record: sorting the names, escaping the
values and joining them measured about 4.5us per increment, against 0.25us for the update it
leads to. A server labelling by route would spend nearly all of its metrics time rebuilding the
same key per request, so resolve the series once -- at start-up, or on the first request for a
route -- and keep it:


```
val get_items = metrics.series("requests_total", Map<string, string> { "route": "/items" })
...
metrics.increment_at(get_items)      // no strings, no sorting, no lookup by name
```

A `Series` stays valid for the life of the process. `reset` invalidates every one of them, which
is another reason it is only for tests.

### `series`

```
pub fn series(name: string, labels: Map<string, string>) -> Series
```

```
pub fn series(name: string) -> Series
```

The unlabelled series of a metric, resolved the same way.

### `series_of`

```
pub fn series_of(key_name: string, key: string) -> Series
```

**A name of its own rather than a third `series` overload**, although the argument types would
tell them apart. `series(name, labels: Map)` sorts and escapes; this one trusts a key the caller
built, and getting that key wrong *splits a series* rather than failing -- so the unsafe form is
opt-in by name, where a `string` second argument beside a `Map` one would make it opt-in by
typo. Same reasoning as `csv`'s `parse_typed`: a name carries what an argument cannot.

A series whose label key is already built, for a caller that knows its labels are safe and in
order and does not want `labels_key`'s sorting and escaping on a hot path. The key is the
rendered label list exactly as it appears between the braces: `k="v",k2="v2"`, names in
increasing order, each value with `\`, `"` and newline escaped. Getting that wrong splits a
series rather than failing, which is why the sorted, escaped `series` is what to reach for first.

### `render`

```
pub fn render() -> string
```

Every declared metric in the Prometheus text format, ending in a newline. Serve this as the
body of a `/metrics` endpoint with `text/plain; version=0.0.4` -- `server.metrics_response`
does that for you.

### `reset`

```
pub fn reset()
```

Forgets every metric and every series. For a test that wants an empty registry; a server has no
reason to call it, and a scraper that sees a counter restart reads it as a process restart.

### `record_runtime`

```
pub fn record_runtime()
```

Samples the runtime's own numbers into gauges, so they render alongside an application's: the
collector's heap bytes and collection count, the live thread count, and the open file
descriptors (docs/roadmap/'s metrics item 3). Call it immediately before `render()` -- these are
sampled, not accumulated, so a value is only as fresh as the last call. `server.metrics_response`
does it for you.

Gauges rather than counters even for the collection count, which is monotonic: a counter in this
module is something *we* add to, and this number is read out of the collector, so modelling it as
a counter would claim an accounting we are not doing. Prometheus is content to compute a rate
over a gauge.

What each answers per build, because the two differ and the difference is not hidden:
`ziman_gc_heap_bytes` is the whole process's mapped heap under the default build and the
*calling thread's* reserved bytes under `nogc`, where the heap is per thread;
`ziman_gc_collections` is -1 under `nogc`, which records no collection count -- -1 rather than 0,
since 0 would read as "none have happened".
