/*
 * gtree - filter the output of `tree` / `tree /F` through a .gitignore
 *
 * Usage:
 *   tree /F | gtree                      (Windows)
 *   tree | gtree                         (Linux/macOS)
 *   gtree -i listing.txt -g ../.gitignore -o filtered.txt
 *
 * See README.md for full documentation, build, and installation
 * instructions for Windows, macOS, and Linux.
 */

#ifndef _WIN32
  #define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <io.h>
  #define gtree_isatty  _isatty
  #define gtree_fileno  _fileno
  #define gtree_popen   _popen
  #define gtree_pclose  _pclose
  #define GTREE_TREE_CMD "tree /F"
#else
  #include <unistd.h>
  #define gtree_isatty  isatty
  #define gtree_fileno  fileno
  #define gtree_popen   popen
  #define gtree_pclose  pclose
  #define GTREE_TREE_CMD "tree"
#endif

#include "gitignore.h"
#include "treeio.h"

#define GTREE_VERSION "1.1.0"

/* Returns the directory part of `path` (handling both '/' and '\\'
 * separators), or "." if `path` has no directory component. Always
 * malloc's a fresh string the caller must free. */
static char *dirname_of_path(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *last = slash;
    size_t len;
    char *out;

    if (bslash && (!last || bslash > last)) last = bslash;
    if (!last) {
        out = (char *)malloc(2);
        if (out) { out[0] = '.'; out[1] = '\0'; }
        return out;
    }

    len = (size_t)(last - path);
    if (len == 0) len = 1; /* "/foo" -> "/" */
    out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

static void print_usage(const char *prog) {
    printf("gtree %s - filter `tree` output through a .gitignore\n\n", GTREE_VERSION);
    printf("Usage:\n");
    printf("  %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -i, --input FILE       Read tree text from FILE (default: stdin)\n");
    printf("  -g, --gitignore FILE   Path to the root .gitignore file\n");
    printf("                         (default: $GTREE_GITIGNORE, or ./.gitignore)\n");
    printf("  -r, --root DIR         Project directory the tree text was generated from.\n");
    printf("                         Used to find nested, per-folder .gitignore files\n");
    printf("                         (e.g. frontend/.gitignore, backend/.gitignore) on\n");
    printf("                         disk and apply them too, just like git itself.\n");
    printf("                         (default: the directory part of -g/--gitignore,\n");
    printf("                         or '.' if -g has none)\n");
    printf("  -o, --output FILE      Write the filtered tree to FILE (default: stdout)\n");
    printf("  -v, --verbose          Print parsing warnings to stderr\n");
    printf("  -h, --help             Show this help and exit\n");
    printf("      --version          Show the version and exit\n\n");
    printf("Examples:\n");
    printf("  tree /F | gtree\n");
    printf("  tree | gtree -g .gitignore\n");
    printf("  gtree -i listing.txt -g .gitignore -r . -o filtered.txt\n");
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    const char *gitignore_path = NULL;
    const char *output_path = NULL;
    const char *root_dir_arg = NULL;
    const char *gitignore_source = NULL;
    char *inferred_root_dir = NULL;
    int verbose = 0;
    int i;

    FILE *in = stdin;
    FILE *out = stdout;
    TreeStyle style = STYLE_WIN_ASCII;
    TreeNode *root;
    IgnoreList *root_ignores;
    IgnoreChain *chain;
    int dirs = 0, files = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(arg, "--version") == 0) {
            printf("gtree %s\n", GTREE_VERSION);
            return 0;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            verbose = 1;
        } else if ((strcmp(arg, "-i") == 0 || strcmp(arg, "--input") == 0) && i + 1 < argc) {
            input_path = argv[++i];
        } else if ((strcmp(arg, "-g") == 0 || strcmp(arg, "--gitignore") == 0) && i + 1 < argc) {
            gitignore_path = argv[++i];
        } else if ((strcmp(arg, "-r") == 0 || strcmp(arg, "--root") == 0) && i + 1 < argc) {
            root_dir_arg = argv[++i];
        } else if ((strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) && i + 1 < argc) {
            output_path = argv[++i];
        } else {
            fprintf(stderr, "gtree: unrecognized argument '%s'\n\n", arg);
            print_usage(argv[0]);
            return 2;
        }
    }

    /* Where the .gitignore path came from, or NULL if nobody asked for a
     * particular one. It matters because a file that is merely absent is
     * normal, while a file somebody named and that is not there is a typo. */
    if (gitignore_path) {
        gitignore_source = "-g";
    } else {
        gitignore_path = getenv("GTREE_GITIGNORE");
        if (gitignore_path) gitignore_source = "GTREE_GITIGNORE";
    }
    if (!gitignore_path) {
        gitignore_path = ".gitignore";
    }

    /* An unreadable .gitignore used to be treated exactly like an empty one:
     * gtree printed the whole tree, filtered nothing, and exited 0. A
     * misspelt -g therefore produced a listing that looked filtered and was
     * not, with nothing on stderr to say so -- the worst possible failure for
     * a tool whose entire job is leaving things out. Absence is still fine
     * when nobody named a file; it is only an error when they did. */
    if (gitignore_source) {
        FILE *probe = fopen(gitignore_path, "rb");
        if (!probe) {
            fprintf(stderr, "gtree: cannot read the .gitignore named by %s: '%s'\n",
                    gitignore_source, gitignore_path);
            fprintf(stderr, "gtree: refusing to print an unfiltered tree that would look filtered.\n");
            return 1;
        }
        fclose(probe);
    }

    if (input_path) {
        in = fopen(input_path, "rb");
        if (!in) {
            fprintf(stderr, "gtree: could not open input file '%s'\n", input_path);
            return 1;
        }
    }

    {
        FILE *auto_pipe = NULL;

        if (!input_path && gtree_isatty(gtree_fileno(stdin))) {
            /* No -i FILE and nothing piped in: rather than block waiting
             * on keyboard input, run `tree` / `tree /F` ourselves. */
            fprintf(stderr, "gtree: no input given; running `%s` for you...\n", GTREE_TREE_CMD);
            auto_pipe = gtree_popen(GTREE_TREE_CMD, "r");
            if (!auto_pipe) {
                fprintf(stderr, "gtree: could not run `%s` (is it installed and on PATH?)\n", GTREE_TREE_CMD);
                fprintf(stderr, "gtree: pipe it in yourself instead, e.g. `%s | gtree`, "
                                "or pass -i FILE.\n", GTREE_TREE_CMD);
                return 1;
            }
            in = auto_pipe;
        }

        root = tree_parse(in, &style, verbose);

        if (auto_pipe) gtree_pclose(auto_pipe);
        else if (in != stdin) fclose(in);
    }

    if (!root) {
        fprintf(stderr, "gtree: no tree data could be parsed from the input\n");
        return 1;
    }

    root_ignores = ignore_list_load(gitignore_path);
    if (!root_ignores) {
        fprintf(stderr, "gtree: out of memory loading '%s'\n", gitignore_path);
        tree_free(root);
        return 1;
    }
    if (verbose) {
        fprintf(stderr, "gtree: using gitignore '%s' (%d pattern%s)\n",
                gitignore_path, root_ignores->count, root_ignores->count == 1 ? "" : "s");
    }

    if (!root_dir_arg) {
        inferred_root_dir = dirname_of_path(gitignore_path);
        root_dir_arg = inferred_root_dir; /* may be NULL on OOM; chain just won't do nested lookups */
    }
    if (verbose && root_dir_arg) {
        fprintf(stderr, "gtree: looking for nested .gitignore files under '%s'\n", root_dir_arg);
    }

    chain = ignore_chain_new(root_dir_arg, root_ignores);
    if (!chain) {
        fprintf(stderr, "gtree: out of memory building ignore chain\n");
        ignore_list_free(root_ignores);
        free(inferred_root_dir);
        tree_free(root);
        return 1;
    }

    tree_filter(root, chain, &dirs, &files);

    if (verbose) {
        fprintf(stderr, "gtree: applied %d nested .gitignore file%s in addition to the root one\n",
                chain->total_nested_hit, chain->total_nested_hit == 1 ? "" : "s");
    }

    /* Nested .gitignore files are found on disk, but the tree text is just
     * text: it may have been captured on another machine or before a rename.
     * When the two disagree the lookups quietly find nothing, and the only
     * sign is rules that appear not to work. Say so once, not per directory,
     * and on stderr so a redirected listing is unaffected. */
    if (chain->dirs_not_on_disk > 0) {
        const char *where = root_dir_arg ? root_dir_arg : ".";
        if (chain->dirs_not_on_disk == 1 && chain->dirs_looked_for == 1) {
            fprintf(stderr,
                    "gtree: warning: the one directory in the listing was not found under\n"
                    "gtree: '%s', so a .gitignore of its own could not be applied.\n", where);
        } else {
            fprintf(stderr,
                    "gtree: warning: %d of the %d directories in the listing were not found\n"
                    "gtree: under '%s', so any .gitignore of their own could not be applied.\n",
                    chain->dirs_not_on_disk, chain->dirs_looked_for, where);
        }
        fprintf(stderr,
                "gtree: Is --root the directory this listing was made from?\n");
    }

    if (output_path) {
        out = fopen(output_path, "wb");
        if (!out) {
            fprintf(stderr, "gtree: could not open output file '%s'\n", output_path);
            ignore_chain_free(chain);
            free(inferred_root_dir);
            tree_free(root);
            return 1;
        }
    }

    tree_render(out, root, style);
    fprintf(out, "\n%d director%s, %d file%s\n",
            dirs, dirs == 1 ? "y" : "ies", files, files == 1 ? "" : "s");

    if (out != stdout) fclose(out);

    ignore_chain_free(chain);
    free(inferred_root_dir);
    tree_free(root);
    return 0;
}
