/*
 * ignore_probe.c - the pattern engine as a one-shot command, so that
 * tests/gitdiff.ps1 can put gtree's answer and real git's answer side by
 * side for the same pattern and the same path.
 *
 *   ignore_probe <gitignore-file> <spec>...
 *
 * where each spec is "f:some/path" for a file or "d:some/path" for a
 * directory. One line of output per spec: "1 <spec>" if the path would be
 * ignored, "0 <spec>" if not.
 *
 * This exists only for the tests. It is not built by `make` or
 * build.bat and is not part of the gtree program.
 */
#include <stdio.h>
#include <string.h>

#include "gitignore.h"

int main(int argc, char **argv) {
    IgnoreList *list;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: ignore_probe <gitignore-file> [f|d]:<path> ...\n");
        return 2;
    }

    list = ignore_list_load(argv[1]);
    if (!list) {
        fprintf(stderr, "ignore_probe: out of memory loading '%s'\n", argv[1]);
        return 2;
    }

    for (i = 2; i < argc; i++) {
        const char *spec = argv[i];
        int is_dir;
        if (strlen(spec) < 3 || spec[1] != ':' || (spec[0] != 'f' && spec[0] != 'd')) {
            fprintf(stderr, "ignore_probe: bad spec '%s'\n", spec);
            ignore_list_free(list);
            return 2;
        }
        is_dir = (spec[0] == 'd');
        printf("%d %s\n", ignore_list_is_ignored(list, spec + 2, is_dir), spec);
    }

    ignore_list_free(list);
    return 0;
}
