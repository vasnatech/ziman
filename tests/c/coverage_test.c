/* Standalone correctness test for src/coverage.c's lcov output and, mainly, its merge: writing into a
 * file that already has records adds to them rather than replacing them, which is what lets a project
 * whose tests are several invocations end up with one honest number (F13). Run via `make test-coverage`.
 *
 * The AST is built by hand rather than parsed, so this links coverage.c and nothing else: a Module with
 * one function whose body has two statements, one that ran and one that did not. That is the whole shape
 * ziman_coverage_accumulate walks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/coverage.h"

static int failures = 0;

static void check(const char *label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

/* The one DA count for `line` in `path`, or -1 when the line is not recorded at all. */
static long long da_count(const char *path, int line) {
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    char buf[1024];
    long long found = -1;
    while (fgets(buf, sizeof buf, f) != NULL) {
        int n;
        long long c;
        if (sscanf(buf, "DA:%d,%lld", &n, &c) == 2 && n == line) found = c;
    }
    fclose(f);
    return found;
}

static int file_has(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    char buf[1024];
    int found = 0;
    while (fgets(buf, sizeof buf, f) != NULL) {
        if (strstr(buf, needle) != NULL) found = 1;
    }
    fclose(f);
    return found;
}

/* One function, body of two statements on lines 2 and 3, with the given execution counts. */
static Module *one_function(const char *file, int ran_first, int ran_second) {
    Stmt *first = calloc(1, sizeof(Stmt));
    first->kind = STMT_RETURN;
    first->line = 2;
    first->exec_count = ran_first;

    Stmt *second = calloc(1, sizeof(Stmt));
    second->kind = STMT_RETURN;
    second->line = 3;
    second->exec_count = ran_second;

    Stmt *body = calloc(1, sizeof(Stmt));
    body->kind = STMT_BLOCK;
    body->line = 1;
    body->as.block.stmts.items = calloc(2, sizeof(Stmt *));
    body->as.block.stmts.items[0] = first;
    body->as.block.stmts.items[1] = second;
    body->as.block.stmts.count = 2;

    FunctionDecl *fn = calloc(1, sizeof(FunctionDecl));
    fn->name = "f";
    fn->source_file = file;
    fn->derived_from = NULL;
    fn->body = body;

    Module *m = calloc(1, sizeof(Module));
    m->functions.items = calloc(1, sizeof(FunctionDecl *));
    m->functions.items[0] = fn;
    m->functions.count = 1;
    return m;
}

/* Releases what one_function built. The real interpreter never frees a Module -- nothing in it has
 * a free function, by design (docs/DESIGN.md's "Memory reclamation") -- but this test built the
 * tree itself with calloc, so it owns it, and freeing it is what lets `make valgrind` leak-check
 * this test rather than skip it (docs/reviews/tasks.md F24). `ziman_coverage_accumulate` only reads
 * the tree, so this is safe once the last check that depends on it has run. */
static void free_one_function(Module *m) {
    FunctionDecl *fn = m->functions.items[0];
    Stmt *body = fn->body;
    for (int i = 0; i < body->as.block.stmts.count; i++) free(body->as.block.stmts.items[i]);
    free(body->as.block.stmts.items);
    free(body);
    free(fn);
    free(m->functions.items);
    free(m);
}

int main(void) {
    const char *path = "/tmp/ziman_coverage_test.info";
    remove(path);

    /* First run: line 2 ran five times, line 3 never. */
    Module *first_run = one_function("subject.zm", 5, 0);
    ziman_coverage_accumulate(first_run);
    check("writing lcov succeeds", ziman_coverage_write_lcov(path) == 0);
    check("the section names the source file", file_has(path, "SF:subject.zm"));
    check("a line that ran carries its count", da_count(path, 2) == 5);
    check("a line that did not run is recorded as 0, not omitted", da_count(path, 3) == 0);
    check("lines found is 2", file_has(path, "LF:2"));
    check("lines hit is 1", file_has(path, "LH:1"));
    check("the record is terminated", file_has(path, "end_of_record"));

    /* Now the merge. This is one process, so the accumulator still holds the first run -- which is
     * *not* what a second invocation looks like, and the counts below say so: line 2 ends at 10, being
     * the 5 still in memory plus the 5 read back from the file. A real second process starts empty and
     * would land on 5. What this pins is the part that is the same either way: the records already in
     * the file are read and added to, so a line this run never touched keeps its earlier count and a
     * line the earlier run never touched gains this one's. The cross-process case was checked by hand
     * (two runs of one program down different branches took LH from 4 to 6). */
    Module *second_run = one_function("subject.zm", 0, 7);
    ziman_coverage_accumulate(second_run);
    check("writing again succeeds", ziman_coverage_write_lcov(path) == 0);
    check("a line this run did not touch keeps its count from the file", da_count(path, 2) == 5 + 5);
    check("a line the file had at 0 gains this run's count", da_count(path, 3) == 7);
    check("lines hit is now 2, from the union", file_has(path, "LH:2"));
    check("still one record per file, not two", !file_has(path, "SF:subject.zmSF:"));

    remove(path);
    free_one_function(first_run);
    free_one_function(second_run);
    remove(path);
    printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
