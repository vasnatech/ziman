/* Standalone test for src/handles.c: a handle set must not hold its lock while it allocates. The collector runs finalizers on the thread that
 * allocates, and a finalizer (net_finalizer, ...) takes its handle out of the set, which takes the lock; held during the allocation, that is a
 * thread waiting for itself. `malloc` and `calloc` are wrapped (see `make test-handles`) so that, while `armed`, each allocation first does what such a
 * finalizer does. With the lock held across the allocation this test hangs and the alarm ends it. Run via `make test-handles`. */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../src/handles.h"

void *__real_malloc(size_t n);
void *__real_calloc(size_t count, size_t size);

static HandleSet set = HANDLE_SET_INIT;
static int armed = 0;
static int finalizer_runs = 0;
static void *victim = (void *)0x1000;

static void run_finalizer(void) {
    if (!armed) return;
    armed = 0; /* the finalizer's own allocations are not re-entered */
    if (handleset_remove(&set, victim)) finalizer_runs++;
    armed = 1;
}

void *__wrap_malloc(size_t n) {
    run_finalizer();
    return __real_malloc(n);
}

/* The compiler turns `malloc` followed by `memset` to zero into `calloc`. */
void *__wrap_calloc(size_t count, size_t size) {
    run_finalizer();
    return __real_calloc(count, size);
}

static void on_alarm(int sig) {
    (void)sig;
    static const char msg[] = "FAIL handleset_add held its lock across an allocation: a finalizer taking the lock deadlocked\n";
    if (write(2, msg, sizeof msg - 1) < 0) _exit(2);
    _exit(1);
}

int main(void) {
    int failures = 0;
    signal(SIGALRM, on_alarm);
    alarm(10);

    handleset_add(&set, victim);
    armed = 1;
    for (uintptr_t i = 1; i <= 500; i++) handleset_add(&set, (void *)(0x2000 + i * 16));
    armed = 0;

    if (finalizer_runs == 0) {
        printf("FAIL the table was never grown, so nothing was tested\n");
        failures++;
    } else {
        printf("PASS a finalizer removed its handle during %d table allocation(s)\n", finalizer_runs);
    }
    if (handleset_has(&set, victim)) {
        printf("FAIL the handle removed by the finalizer is still in the set\n");
        failures++;
    } else {
        printf("PASS the removed handle is gone\n");
    }
    int missing = 0;
    for (uintptr_t i = 1; i <= 500; i++) {
        if (!handleset_has(&set, (void *)(0x2000 + i * 16))) missing++;
    }
    if (missing != 0) {
        printf("FAIL %d of 500 added handles are missing after the table grew\n", missing);
        failures++;
    } else {
        printf("PASS all 500 added handles are present\n");
    }
    printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
