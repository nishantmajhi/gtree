/*
 * treeio_test.c - parsing, filtering and re-rendering tree text.
 *
 *   treeio_test <fixtures-dir>
 *
 * The four listings this reads are in tests/fixtures. The two Windows ones
 * are the byte-exact output of `tree /F` and `tree /F /A` on Windows 11,
 * captured by redirecting to a file; they are read from disk rather than
 * copied into this file so that there is only one version of them.
 *
 * What matters about them, and what the hand-written sample listings in
 * the project's old test/ folder got wrong, is this:
 *
 *   - `tree /F` gives a *file* no connector glyph at all. Files are
 *     indented with continuation groups only:
 *
 *         C:.
 *         |   README.md            <- a file at depth 1, no "+---"
 *         +---build
 *         |       out.bin          <- a file at depth 2
 *
 *   - when the output is not a console, the vertical bar is written as
 *     the single byte 0xA6, not as ASCII '|' and not as UTF-8 U+2502.
 *     `tree /F /A` is the variant that emits ASCII '|'.
 *
 *   - both variants emit "spacer" lines carrying indentation and nothing
 *     else ("|   ", trailing spaces included).
 *
 * The original code handled none of those three facts, so nearly everything
 * the parser does with them is a regression test. Checks named "D1: ...",
 * "D2: ..." and so on describe the correspondingly numbered defect in
 * review.md; tests/run.ps1 -Original asserts that each of those numbers has
 * at least one check that fails against the original sources.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define MKDIR(p) mkdir((p), 0777)
#endif

#include "harness.h"
#include "treeio.h"
#include "gitignore.h"

#define TMPDIR "gtree_tmp_treeio"

/* The vertical bar byte that a redirected `tree /F` actually writes. */
#define BAR "\xA6"

static const char *g_fixtures = "fixtures";

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "could not write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}

/* Parses one of the committed fixture listings. */
static TreeNode *parse_fixture(const char *file, TreeStyle *style) {
    char path[1024];
    FILE *f;
    TreeNode *root;
    TreeStyle local = STYLE_UNKNOWN;

    snprintf(path, sizeof path, "%s/%s", g_fixtures, file);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "could not open fixture %s\n", path); exit(2); }
    root = tree_parse(f, style ? style : &local, 0);
    fclose(f);
    return root;
}

/* Parses a listing written on the fly, for the shapes no capture covers. */
static TreeNode *parse_text(const char *text, TreeStyle *style) {
    FILE *f;
    TreeNode *root;
    TreeStyle local = STYLE_UNKNOWN;
    write_text(TMPDIR "/listing.txt", text);
    f = fopen(TMPDIR "/listing.txt", "rb");
    if (!f) exit(2);
    root = tree_parse(f, style ? style : &local, 0);
    fclose(f);
    return root;
}

/* Renders a filtered tree into a malloc'd string. Caller frees. */
static char *render_to_string(TreeNode *root, TreeStyle style) {
    FILE *f = fopen(TMPDIR "/out.txt", "wb");
    long len;
    char *buf;
    if (!f) { fprintf(stderr, "could not write output\n"); exit(2); }
    tree_render(f, root, style);
    fclose(f);

    f = fopen(TMPDIR "/out.txt", "rb");
    if (!f) exit(2);
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) exit(2);
    len = (long)fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Finds a node by its path below root, e.g. "src/utils". */
static TreeNode *find_node(TreeNode *root, const char *path) {
    const char *p = path;
    TreeNode *cur = root;

    if (!root) return NULL;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seg = slash ? (size_t)(slash - p) : strlen(p);
        TreeNode *c, *hit = NULL;
        for (c = cur->first_child; c; c = c->next_sibling) {
            if (strlen(c->name) == seg && strncmp(c->name, p, seg) == 0) { hit = c; break; }
        }
        if (!hit) return NULL;
        cur = hit;
        p = slash ? slash + 1 : p + seg;
    }
    return cur;
}

/* The keep flag of a node, or -1 if the node is not in the tree at all.
 * Every filtering assertion goes through this rather than dereferencing
 * find_node() directly: against the original code the interesting nodes
 * are missing, and a suite that segfaults on the first one tells you far
 * less than one that reports which checks failed. */
static int keep_of(TreeNode *root, const char *path) {
    TreeNode *n = find_node(root, path);
    return n ? n->keep : -1;
}

/* Counts every node in the tree whose name equals `name`. */
static int count_named(const TreeNode *node, const char *name) {
    const TreeNode *c;
    int n = 0;
    if (!node) return 0;
    for (c = node->first_child; c; c = c->next_sibling) {
        if (strcmp(c->name, name) == 0) n++;
        n += count_named(c, name);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Structure: what the parser makes of each dialect                    */
/* ------------------------------------------------------------------ */

/* All four fixtures describe the same tree, so one set of expectations
 * serves for all of them. `label` distinguishes the check names. */
static void check_structure(const char *label, const char *fixture, const char *root_name) {
    char name[160];
    TreeNode *root = parse_fixture(fixture, NULL);
    TreeNode *n;

    snprintf(name, sizeof name, "%s: parses a root", label);
    check(name, root != NULL);
    if (!root) return;

    snprintf(name, sizeof name, "%s: the root line is the path, not a header", label);
    check_str(name, root_name, root->name);

    /* D1: a spacer line ("|   " with its trailing spaces trimmed off)
     * became an entry whose name was a lone vertical bar, and everything
     * indented below it was parented to that phantom directory. */
    snprintf(name, sizeof name, "D1: %s invents no entry named '|'", label);
    check_int(name, 0, count_named(root, "|"));
    snprintf(name, sizeof name, "D1: %s invents no entry named with the bar byte", label);
    check_int(name, 0, count_named(root, BAR));

    /* D2: a file line carries no connector, and in the default Windows
     * dialect its indentation starts with a byte the parser did not know,
     * so the whole line was dropped. */
    snprintf(name, sizeof name, "D2: %s keeps a file at the root", label);
    check(name, find_node(root, "README.md") != NULL);
    snprintf(name, sizeof name, "D2: %s keeps the second root file", label);
    check(name, find_node(root, "debug.log") != NULL);

    snprintf(name, sizeof name, "D2: %s puts a file under its own directory", label);
    check(name, find_node(root, "build/out.bin") != NULL);
    snprintf(name, sizeof name, "D2: %s therefore knows build is a directory", label);
    n = find_node(root, "build");
    check(name, n != NULL && n->is_dir);

    snprintf(name, sizeof name, "D2: %s reads a directory holding only a directory", label);
    n = find_node(root, "node_modules/pkg");
    check(name, n != NULL && n->is_dir);
    snprintf(name, sizeof name, "D2: %s reaches a file two levels down", label);
    check(name, find_node(root, "node_modules/pkg/p.js") != NULL);

    snprintf(name, sizeof name, "D2: %s keeps files beside a subdirectory", label);
    check(name, find_node(root, "src/index.js") != NULL && find_node(root, "src/app.log") != NULL);
    snprintf(name, sizeof name, "D2: %s keeps the deepest file", label);
    check(name, find_node(root, "src/utils/helpers.js") != NULL);

    snprintf(name, sizeof name, "%s: an empty directory has no children", label);
    n = find_node(root, "empty");
    check(name, n != NULL && n->first_child == NULL);

    snprintf(name, sizeof name, "D2: %s yields the whole tree and nothing more", label);
    {
        /* seven files and six directories, counted as nodes */
        int total = 0;
        const char *names[] = { "README.md", "debug.log", "build", "build/out.bin", "empty",
                                "node_modules", "node_modules/pkg", "node_modules/pkg/p.js",
                                "src", "src/app.log", "src/index.js", "src/utils",
                                "src/utils/helpers.js" };
        size_t i;
        for (i = 0; i < sizeof names / sizeof names[0]; i++) {
            if (find_node(root, names[i])) total++;
        }
        check_int(name, 13, total);
    }

    tree_free(root);
}

static void style_detection(void) {
    TreeStyle style = STYLE_UNKNOWN;
    TreeNode *root;

    root = parse_fixture("win_tree_f.txt", &style);
    check_int("style: default tree /F reads as Windows", STYLE_WIN_ASCII, style);
    tree_free(root);

    style = STYLE_UNKNOWN;
    root = parse_fixture("win_tree_f_ascii.txt", &style);
    check_int("style: tree /F /A reads as Windows", STYLE_WIN_ASCII, style);
    tree_free(root);

    style = STYLE_UNKNOWN;
    root = parse_fixture("unix_utf8.txt", &style);
    check_int("style: unix tree reads as UTF-8", STYLE_UNIX_UTF8, style);
    check("unix utf-8: the footer is not an entry",
          find_node(root, "6 directories, 7 files") == NULL);
    tree_free(root);

    style = STYLE_UNKNOWN;
    root = parse_fixture("unix_ascii.txt", &style);
    check_int("style: tree -A reads as Unix ASCII", STYLE_UNIX_ASCII, style);
    tree_free(root);
}

/* `tree -F` marks directories with a trailing '/', which removes the
 * guesswork, and marks other kinds of file with a suffix to strip. */
static void classifier_suffixes(void) {
    TreeNode *root = parse_text(
        ".\n"
        "|-- empty/\n"
        "|-- run.sh*\n"
        "|-- link@\n"
        "`-- sock=\n", NULL);
    TreeNode *n;
    check("tree -F: a trailing slash names a directory",
          (n = find_node(root, "empty")) != NULL && n->is_dir);
    check("tree -F: an executable keeps its name", find_node(root, "run.sh") != NULL);
    check("tree -F: an executable is not a directory",
          (n = find_node(root, "run.sh")) != NULL && !n->is_dir);
    check("tree -F: a symlink keeps its name", find_node(root, "link") != NULL);
    check("tree -F: a socket keeps its name", find_node(root, "sock") != NULL);
    tree_free(root);
}

static void malformed_input(void) {
    TreeNode *root;

    root = parse_text("", NULL);
    check("malformed: empty input yields no tree", root == NULL);

    root = parse_text("Folder PATH listing\r\nVolume serial number is 1A3C-B410\r\n", NULL);
    check("malformed: headers alone yield no tree", root == NULL);
    tree_free(root);

    root = parse_text("+---orphan\r\n", NULL);
    check("malformed: an entry with no root yields no tree", root == NULL);
    tree_free(root);

    root = parse_text("just-a-root\r\n", NULL);
    check("malformed: a lone root parses", root != NULL && root->first_child == NULL);
    if (root) check_str("malformed: the lone root keeps its name", "just-a-root", root->name);
    tree_free(root);

    /* D8: the root line was the first line that did not match one of three
     * English phrases, so on a Windows whose tree speaks another language
     * the header became the root and the real root line was thrown away. */
    root = parse_text(
        "Auflistung der Ordnerpfade fuer Volume OS\r\n"
        "Volumeseriennummer: 1A3C-B410\r\n"
        "C:.\r\n"
        "+---src\r\n", NULL);
    check_str("D8: a non-English header is not mistaken for the root",
              "C:.", root ? root->name : "(no tree)");
    check("D8: the entry below a non-English header still parses",
          find_node(root, "src") != NULL);
    tree_free(root);
}

/* ------------------------------------------------------------------ */
/* Filtering and rendering                                             */
/* ------------------------------------------------------------------ */

static IgnoreChain *chain_from(const char *gitignore_text, const char *root_dir) {
    IgnoreList *list;
    IgnoreChain *chain;
    write_text(TMPDIR "/rules.gitignore", gitignore_text);
    list = ignore_list_load(TMPDIR "/rules.gitignore");
    chain = ignore_chain_new(root_dir, list);
    if (!chain) { fprintf(stderr, "out of memory\n"); exit(2); }
    return chain;
}

static void filter_and_render(const char *label, const char *fixture,
                              const char *expected) {
    char name[160];
    TreeStyle style = STYLE_UNKNOWN;
    TreeNode *root = parse_fixture(fixture, &style);
    IgnoreChain *chain = chain_from("build/\n*.log\nnode_modules/\n", NULL);
    int dirs = -1, files = -1;
    char *rendered;

    tree_filter(root, chain, &dirs, &files);

    /* D2 again, from the other end: because every file line was dropped,
     * build and node_modules had no children, were not recognised as
     * directories, and so a directory-only rule stopped matching them. An
     * ignore rule failing silently is the worst of the consequences. */
    snprintf(name, sizeof name, "D2: %s honours build/ on a directory of files", label);
    check_int(name, 0, keep_of(root, "build"));
    snprintf(name, sizeof name, "D2: %s honours node_modules/", label);
    check_int(name, 0, keep_of(root, "node_modules"));
    snprintf(name, sizeof name, "D2: %s drops an ignored root file", label);
    check_int(name, 0, keep_of(root, "debug.log"));
    snprintf(name, sizeof name, "D2: %s keeps what is not ignored", label);
    check_int(name, 1, keep_of(root, "README.md"));

    snprintf(name, sizeof name, "D2: %s counts the kept directories", label);
    check_int(name, 2, dirs); /* src, src/utils; "empty" reads as a file */
    snprintf(name, sizeof name, "D2: %s counts the kept files", label);
    check_int(name, 4, files); /* README.md, empty, index.js, helpers.js */

    rendered = render_to_string(root, style);
    snprintf(name, sizeof name, "D2: %s re-renders the filtered tree", label);
    check_str(name, expected, rendered);
    free(rendered);

    ignore_chain_free(chain);
    tree_free(root);
}

/* An ignored directory takes its contents with it, and .git goes
 * unconditionally. */
static void pruning(void) {
    TreeNode *root = parse_text(
        ".\n"
        "|-- .git\n"
        "|   `-- config\n"
        "|-- build\n"
        "|   `-- keep.txt\n"
        "`-- src\n"
        "    `-- a.c\n", NULL);
    IgnoreChain *chain = chain_from("build/\n!build/keep.txt\n", NULL);
    int dirs = 0, files = 0;
    char *rendered;

    tree_filter(root, chain, &dirs, &files);
    check_int("prune: .git is dropped without a rule", 0, keep_of(root, ".git"));
    check_int("prune: an ignored directory is dropped", 0, keep_of(root, "build"));
    check_int("D9: the keep flag is cleared throughout a pruned subtree", 0,
              keep_of(root, "build/keep.txt"));
    check_int("prune: the counts exclude pruned subtrees", 1, dirs);
    check_int("prune: one file survives", 1, files);

    rendered = render_to_string(root, STYLE_UNIX_ASCII);
    check_str("prune: nothing pruned is drawn",
              ".\n"
              "`-- src\n"
              "    `-- a.c\n",
              rendered);
    free(rendered);

    ignore_chain_free(chain);
    tree_free(root);
}

/* Dropping the last entry has to promote the one before it, or the tree is
 * drawn with a branch that leads nowhere. */
static void render_last_child(void) {
    TreeNode *root = parse_fixture("unix_utf8.txt", NULL);
    IgnoreChain *chain = chain_from("src/\n", NULL);
    int dirs = 0, files = 0;
    char *rendered;

    tree_filter(root, chain, &dirs, &files);
    rendered = render_to_string(root, STYLE_UNIX_UTF8);
    check_str("render: the new last entry gets the corner connector",
              ".\n"
              "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 README.md\n"
              "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 build\n"
              "\xE2\x94\x82   \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 out.bin\n"
              "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 debug.log\n"
              "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 empty\n"
              "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 node_modules\n"
              "    \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 pkg\n"
              "        \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 p.js\n",
              rendered);
    free(rendered);
    ignore_chain_free(chain);
    tree_free(root);
}

/* Nested .gitignore files are found on disk relative to --root, so the
 * relative paths the filter builds have to be right. When a phantom node
 * was inserted into the tree (D1), every path below it gained a segment
 * and every nested lookup missed. */
static void nested_on_disk(void) {
    TreeNode *root;
    IgnoreChain *chain;
    int dirs = 0, files = 0;

    MKDIR(TMPDIR "/proj");
    MKDIR(TMPDIR "/proj/src");
    write_text(TMPDIR "/proj/src/.gitignore", "!*.log\n*.js\n");

    root = parse_fixture("win_tree_f_ascii.txt", NULL);
    chain = chain_from("*.log\n", TMPDIR "/proj");
    tree_filter(root, chain, &dirs, &files);

    check_int("D1: a nested .gitignore is found for a Windows listing", 1,
              chain->total_nested_hit);
    check_int("D1: a nested rule is applied", 0, keep_of(root, "src/index.js"));
    check_int("D1: it reaches further down as well", 0,
              keep_of(root, "src/utils/helpers.js"));

    /* D6: git gives the deepest .gitignore the last word, so src's
     * "!*.log" re-includes a file the root .gitignore excluded. The
     * original checked levels root-first and returned on the first one
     * that ignored the path, so a nested negation could never win. */
    check_int("D6: a nested negation overrides a root rule", 1,
              keep_of(root, "src/app.log"));
    check_int("nested: the root rule still applies outside that scope", 0,
              keep_of(root, "debug.log"));
    check_int("nested: a nested rule does not leak upwards", 1, keep_of(root, "README.md"));

    ignore_chain_free(chain);
    tree_free(root);
}

int main(int argc, char **argv) {
    if (argc > 1) g_fixtures = argv[1];
    h_init("treeio");
    MKDIR(TMPDIR);

    check_structure("tree /F", "win_tree_f.txt", "C:.");
    check_structure("tree /F /A", "win_tree_f_ascii.txt", "C:.");
    check_structure("tree", "unix_utf8.txt", ".");
    check_structure("tree -A", "unix_ascii.txt", ".");

    style_detection();
    classifier_suffixes();
    malformed_input();

    filter_and_render("tree /F", "win_tree_f.txt",
                      "C:.\n"
                      "+---README.md\n"
                      "+---empty\n"
                      "\\---src\n"
                      "    +---index.js\n"
                      "    \\---utils\n"
                      "        \\---helpers.js\n");
    filter_and_render("tree /F /A", "win_tree_f_ascii.txt",
                      "C:.\n"
                      "+---README.md\n"
                      "+---empty\n"
                      "\\---src\n"
                      "    +---index.js\n"
                      "    \\---utils\n"
                      "        \\---helpers.js\n");
    filter_and_render("tree", "unix_utf8.txt",
                      ".\n"
                      "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 README.md\n"
                      "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 empty\n"
                      "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 src\n"
                      "    \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 index.js\n"
                      "    \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 utils\n"
                      "        \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 helpers.js\n");
    filter_and_render("tree -A", "unix_ascii.txt",
                      ".\n"
                      "|-- README.md\n"
                      "|-- empty\n"
                      "`-- src\n"
                      "    |-- index.js\n"
                      "    `-- utils\n"
                      "        `-- helpers.js\n");

    render_last_child();
    pruning();
    nested_on_disk();
    return h_report();
}
