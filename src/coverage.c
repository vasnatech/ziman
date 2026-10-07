#define _POSIX_C_SOURCE 200809L
#include "coverage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One file's lines, as a growable set of (line, ran) pairs kept sorted by line. A program's own
 * source is a handful of files with a few thousand lines between them, so a linear insert into a
 * sorted array is both simpler and faster here than a hash table would be -- and it hands the
 * report back in line order for free. */
typedef struct {
    int line;
    /* How many times a statement on this line ran, summed over the statements on it and over every
     * module accumulated. The text report only needs "did it run", but lcov's `DA:<line>,<count>`
     * record wants the count, and `Stmt.exec_count` already holds it, so nothing extra is counted at
     * run time to have it. */
    long long count;
} LineHit;

typedef struct {
    const char *file;
    LineHit *lines;
    int count, capacity;
} FileCoverage;

typedef struct {
    FileCoverage *files;
    int count, capacity;
} Coverage;

int ziman_coverage_on = 0;
/* Accumulated across every module handed to ziman_coverage_accumulate -- see the header on why
 * `ziman test` needs that rather than a report per module. */
static Coverage g_cov = {0};

void ziman_coverage_enable(void) { ziman_coverage_on = 1; }

static FileCoverage *file_for(Coverage *cov, const char *file) {
    for (int i = 0; i < cov->count; i++) {
        if (strcmp(cov->files[i].file, file) == 0) return &cov->files[i];
    }
    if (cov->count == cov->capacity) {
        cov->capacity = cov->capacity == 0 ? 4 : cov->capacity * 2;
        cov->files = realloc(cov->files, (size_t)cov->capacity * sizeof(FileCoverage));
    }
    FileCoverage *fc = &cov->files[cov->count++];
    fc->file = file;
    fc->lines = NULL;
    fc->count = 0;
    fc->capacity = 0;
    return fc;
}

/* Records that `line` is executable, and that it ran if `ran`. Several statements can start on one
 * line (`if x { return 1 }`), so a line already present is updated rather than added again: it is
 * covered if *any* statement on it ran, which is the only reading that doesn't punish putting two
 * statements on a line. */
static void note_line(FileCoverage *fc, int line, long long count) {
    int lo = 0, hi = fc->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (fc->lines[mid].line < line) lo = mid + 1;
        else hi = mid;
    }
    if (lo < fc->count && fc->lines[lo].line == line) {
        fc->lines[lo].count += count;
        return;
    }
    if (fc->count == fc->capacity) {
        fc->capacity = fc->capacity == 0 ? 32 : fc->capacity * 2;
        fc->lines = realloc(fc->lines, (size_t)fc->capacity * sizeof(LineHit));
    }
    memmove(fc->lines + lo + 1, fc->lines + lo, (size_t)(fc->count - lo) * sizeof(LineHit));
    fc->lines[lo].line = line;
    fc->lines[lo].count = count;
    fc->count++;
}

/* Every statement in the tree rooted at `s`. The nesting cases are exactly the statement kinds that
 * hold another statement (ast.h's own Stmt union says which); everything else is a leaf, and a leaf
 * that gains children later would simply stop being visited, so new nesting belongs here too. */
static void walk_stmt(FileCoverage *fc, const Stmt *s) {
    if (s == NULL) return;
    /* A STMT_BLOCK's own line is a `{`, not something that runs: a function body's block carries
     * the `fn` line, and a bare block carries its own brace. Counting it would report every
     * function's signature line as uncovered, because a body's statements are run through
     * exec_stmts_in_scope and the block itself never passes through exec_stmt at all. Its children
     * are what matter, and they are walked below. */
    if (s->kind != STMT_BLOCK) note_line(fc, s->line, (long long)s->exec_count);
    switch (s->kind) {
        case STMT_IF:
            walk_stmt(fc, s->as.if_stmt.then_block);
            walk_stmt(fc, s->as.if_stmt.else_block);
            break;
        case STMT_WHILE:
            walk_stmt(fc, s->as.while_stmt.body);
            break;
        case STMT_FOR:
            walk_stmt(fc, s->as.for_stmt.body);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) walk_stmt(fc, s->as.block.stmts.items[i]);
            break;
        case STMT_SWITCH:
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) {
                walk_stmt(fc, s->as.switch_stmt.arms[i].body);
            }
            break;
        case STMT_VALUE_BLOCK:
            walk_stmt(fc, s->as.value_block.inner);
            break;
        default:
            break; /* a leaf: no nested statements to visit */
    }
}

void ziman_coverage_accumulate(const Module *module) {
    for (int i = 0; i < module->functions.count; i++) {
        const FunctionDecl *fn = module->functions.items[i];
        /* The same "is this the user's own code" test --lint uses (typecheck.c). A native fn has no
         * body to walk either way. */
        if (fn->source_file == NULL || fn->derived_from != NULL || fn->body == NULL) continue;
        walk_stmt(file_for(&g_cov, fn->source_file), fn->body);
    }
}

/* ------------------------------------------------------------- lcov output
 *
 * The `.info` format `lcov`, `genhtml`, Codecov and Coveralls all read:
 *
 *     TN:                      the test name, empty -- there is one run, not a named suite
 *     SF:<path>                one section per source file
 *     DA:<line>,<count>        one per executable line, with how many times it ran
 *     LF:<n>  LH:<n>           lines found, lines hit
 *     end_of_record
 *
 * **Merging is done here, not left to the reader.** If the target file already exists, its records
 * are read and this run's counts are added to them, so running several times into one path
 * accumulates -- which is what a project needs when its tests are several invocations rather than
 * one. The alternative was to write a fresh file each time and tell people that concatenating two
 * `.info` files merges them because lcov sums repeated records; that is true as far as I know, but
 * no lcov tool is installed on the machine this was written on, so it would have been a claim about
 * software I could not run. Merging here is a claim about code in this file, which its own fixture
 * checks: two runs covering different lines produce the union.
 *
 * Delete the file to start a fresh measurement. A line's count can only grow, so a stale file makes
 * coverage look better than it is -- the one failure mode worth knowing about, and the reason the
 * path is always named explicitly by the caller rather than defaulted.
 *
 * Paths are written exactly as the compiler saw them, which is what the person ran; a relative one
 * stays relative, since the tools resolve it against where they are invoked. */
static void merge_existing_lcov(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) return;
    char line[4096];
    FileCoverage *fc = NULL;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "SF:", 3) == 0) {
            char *nl = strchr(line + 3, '\n');
            if (nl != NULL) *nl = '\0';
            /* strdup because the Coverage keeps the pointer and this buffer is reused per line. */
            fc = file_for(&g_cov, strdup(line + 3));
        } else if (strncmp(line, "DA:", 3) == 0 && fc != NULL) {
            char *comma = strchr(line + 3, ',');
            if (comma == NULL) continue;
            *comma = '\0';
            note_line(fc, atoi(line + 3), atoll(comma + 1));
        } else if (strncmp(line, "end_of_record", 13) == 0) {
            fc = NULL;
        }
    }
    fclose(f);
}

int ziman_coverage_write_lcov(const char *path) {
    /* Read first, then rewrite: the merge is this run's counts plus whatever the file already held. */
    merge_existing_lcov(path);
    FILE *f = fopen(path, "w");
    if (f == NULL) return -1;
    for (int fi = 0; fi < g_cov.count; fi++) {
        FileCoverage *fc = &g_cov.files[fi];
        int hit = 0;
        for (int i = 0; i < fc->count; i++) {
            if (fc->lines[i].count > 0) hit++;
        }
        fprintf(f, "TN:\nSF:%s\n", fc->file);
        for (int i = 0; i < fc->count; i++) {
            fprintf(f, "DA:%d,%lld\n", fc->lines[i].line, fc->lines[i].count);
        }
        fprintf(f, "LF:%d\nLH:%d\nend_of_record\n", fc->count, hit);
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

int ziman_coverage_report(void) {
    Coverage *cov = &g_cov;
    int total = 0, covered = 0;
    for (int f = 0; f < cov->count; f++) {
        FileCoverage *fc = &cov->files[f];
        int file_total = fc->count, file_covered = 0;
        for (int i = 0; i < fc->count; i++) {
            if (fc->lines[i].count > 0) file_covered++;
        }
        total += file_total;
        covered += file_covered;
        fprintf(stderr, "%s: %d/%d lines (%.0f%%)\n", fc->file, file_covered, file_total,
                file_total > 0 ? 100.0 * file_covered / file_total : 100.0);
        if (file_covered < file_total) {
            fprintf(stderr, "  not run:");
            for (int i = 0; i < fc->count; i++) {
                if (fc->lines[i].count == 0) fprintf(stderr, " %d", fc->lines[i].line);
            }
            fprintf(stderr, "\n");
        }
    }
    if (cov->count == 0) {
        fprintf(stderr, "coverage: no user code to report (every function came from a bundled module)\n");
    } else if (cov->count > 1) {
        fprintf(stderr, "total: %d/%d lines (%.0f%%)\n", covered, total,
                total > 0 ? 100.0 * covered / total : 100.0);
    }

    for (int f = 0; f < cov->count; f++) free(cov->files[f].lines);
    free(cov->files);
    cov->files = NULL;
    cov->count = 0;
    cov->capacity = 0;
    return total - covered;
}
