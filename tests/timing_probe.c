/*
 * timing_probe.c - does gtree_glob_match() finish?
 *
 *   timing_probe <pattern> <text>
 *
 * Prints the verdict and the elapsed milliseconds, then exits 0. This is
 * a separate program rather than a check inside a suite because the defect
 * it exists for is non-termination: the original matcher backtracks
 * exponentially, so a pattern like "*a*a*a*a*a*a*a*a*a*a*b" against a run
 * of fifty a's does not come back. A check cannot report a hang from
 * inside the process that is hanging, so tests/run.ps1 runs this with a
 * timeout instead, and asserts that the fixed matcher answers and the
 * original one does not.
 */
#include <stdio.h>
#include <time.h>

#include "gitignore.h"

int main(int argc, char **argv) {
    clock_t t0, t1;
    int matched;

    if (argc != 3) {
        fprintf(stderr, "usage: timing_probe <pattern> <text>\n");
        return 2;
    }

    t0 = clock();
    matched = gtree_glob_match(argv[1], argv[2]);
    t1 = clock();

    printf("%s in %.0f ms\n", matched ? "match" : "no match",
           1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC);
    return 0;
}
