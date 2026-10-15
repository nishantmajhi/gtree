/*
 * harness.h - a dependency-free assertion harness for gtree's tests.
 *
 * Every check prints one machine-readable line, "PASS <name>" or
 * "FAIL <name>", so the runner can do more than look at an exit code.
 * That matters because these suites are also compiled against the
 * *original* sources (tests/run.ps1 -Original), where the interesting
 * question is not "did it fail" but "did exactly the checks that name a
 * defect fail". Checks whose name begins with "regression:" are the ones
 * that must fail against the original code; run.ps1 asserts that.
 *
 * Output is unbuffered. A suite run against unfixed code is exactly when
 * the process dies partway through, and which check it reached is the
 * part worth knowing.
 */
#ifndef GTREE_TEST_HARNESS_H
#define GTREE_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int h_run = 0;
static int h_failed = 0;

static inline void h_init(const char *suite) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("# suite %s\n", suite);
}

static inline void h_ok(const char *name) {
    h_run++;
    printf("PASS %s\n", name);
}

static inline void h_bad(const char *name, const char *detail) {
    h_run++;
    h_failed++;
    printf("FAIL %s%s%s\n", name, detail && *detail ? " -- " : "", detail ? detail : "");
}

static inline void check(const char *name, int cond) {
    if (cond) h_ok(name); else h_bad(name, "condition was false");
}

static inline void check_int(const char *name, int expect, int actual) {
    if (expect == actual) {
        h_ok(name);
    } else {
        char buf[128];
        snprintf(buf, sizeof buf, "expected %d, got %d", expect, actual);
        h_bad(name, buf);
    }
}

static inline void check_str(const char *name, const char *expect, const char *actual) {
    if (expect && actual && strcmp(expect, actual) == 0) {
        h_ok(name);
    } else {
        char buf[1024];
        snprintf(buf, sizeof buf, "expected [%s], got [%s]",
                 expect ? expect : "(null)", actual ? actual : "(null)");
        h_bad(name, buf);
    }
}

static inline int h_report(void) {
    printf("# %d checks, %d failed\n", h_run, h_failed);
    return h_failed == 0 ? 0 : 1;
}

#endif /* GTREE_TEST_HARNESS_H */
