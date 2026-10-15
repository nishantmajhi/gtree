/*
 * gitignore_test.c - the .gitignore pattern engine and the nested chain.
 *
 * Every expectation about pattern semantics in here was taken from real
 * git, not from reading gtree's code: each one was checked with
 * `git check-ignore --no-index` in a throwaway repository with
 * core.ignorecase=false and core.excludesFile=/dev/null. tests/run.ps1
 * re-derives a subset of them from the git on this machine at run time
 * (see tests/gitdiff.ps1), so the two cannot drift apart silently.
 *
 * Checks named "D3: ...", "D7: ..." and so on describe the correspondingly
 * numbered defect in review.md, and are expected to fail against the
 * original code. Run tests/run.ps1 -Original to watch them do it.
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
#include "gitignore.h"

#define TMPDIR "gtree_tmp_ignore"

static void write_bytes(const char *path, const char *bytes, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "could not write %s\n", path); exit(2); }
    fwrite(bytes, 1, len, f);
    fclose(f);
}

static void write_text(const char *path, const char *text) {
    write_bytes(path, text, strlen(text));
}

/* Loads a list from a .gitignore written on the fly. */
static IgnoreList *list_from(const char *text) {
    IgnoreList *l;
    write_text(TMPDIR "/.gitignore", text);
    l = ignore_list_load(TMPDIR "/.gitignore");
    if (!l) { fprintf(stderr, "out of memory\n"); exit(2); }
    return l;
}

/* One pattern, one path: does the engine ignore it? */
static void expect_ignore(const char *name, const char *gitignore,
                          const char *path, int is_dir, int expect) {
    IgnoreList *l = list_from(gitignore);
    check_int(name, expect, ignore_list_is_ignored(l, path, is_dir));
    ignore_list_free(l);
}

static void glob_cases(void) {
    /* --- the matcher on its own ------------------------------------ */
    check("glob: literal", gtree_glob_match("abc", "abc"));
    check("glob: literal mismatch", !gtree_glob_match("abc", "abd"));
    check("glob: star does not cross slash", !gtree_glob_match("a*b", "a/b"));
    check("glob: star within a segment", gtree_glob_match("a*b", "axyb"));
    check("glob: star matches empty", gtree_glob_match("a*b", "ab"));
    check("glob: question mark", gtree_glob_match("?.txt", "a.txt"));
    check("glob: question mark is exactly one", !gtree_glob_match("?.txt", "ab.txt"));
    check("glob: question mark does not cross slash", !gtree_glob_match("a?b", "a/b"));
    check("glob: class", gtree_glob_match("[abc].txt", "b.txt"));
    check("glob: class excludes", !gtree_glob_match("[abc].txt", "d.txt"));
    check("glob: range", gtree_glob_match("[a-z].txt", "q.txt"));
    check("glob: range is case sensitive", !gtree_glob_match("[a-z].txt", "Q.txt"));
    check("glob: negated class with !", !gtree_glob_match("[!a].txt", "a.txt"));
    check("glob: negated class with ! admits others", gtree_glob_match("[!a].txt", "b.txt"));
    check("glob: negated class with ^", !gtree_glob_match("[^a].txt", "a.txt"));
    check("glob: literal ] first in class", gtree_glob_match("[]].txt", "].txt"));
    check("glob: unterminated class never matches", !gtree_glob_match("[abc.txt", "a.txt"));
    check("glob: class does not match a slash", !gtree_glob_match("a[b/]c", "a/c"));
    check("glob: escaped star is literal", gtree_glob_match("a\\*b", "a*b"));
    check("glob: escaped star is not a wildcard", !gtree_glob_match("a\\*b", "axb"));
    check("glob: globstar crosses slashes", gtree_glob_match("a/**/b", "a/x/y/b"));
    check("glob: globstar matches zero segments", gtree_glob_match("a/**/b", "a/b"));
    check("glob: leading globstar at depth", gtree_glob_match("**/foo", "a/b/foo"));
    check("glob: leading globstar at root", gtree_glob_match("**/foo", "foo"));
    check("glob: trailing globstar", gtree_glob_match("foo/**", "foo/a/b"));
    check("glob: collapsed stars behave as globstar", gtree_glob_match("a***b", "a/b"));
    check("glob: whole-string match only", !gtree_glob_match("abc", "abcd"));

    /* --- patterns as .gitignore lines ------------------------------ */
    expect_ignore("rule: basename at any depth", "*.log", "a/b/c.log", 0, 1);
    expect_ignore("rule: basename does not match a suffix", "*.log", "c.log.txt", 0, 0);
    expect_ignore("rule: directory-only matches a directory", "build/", "build", 1, 1);
    expect_ignore("rule: directory-only skips a file", "build/", "build", 0, 0);
    expect_ignore("rule: directory-only at depth", "build/", "a/build", 1, 1);
    expect_ignore("rule: root anchor matches at root", "/config.json", "config.json", 0, 1);
    expect_ignore("rule: root anchor does not match at depth", "/config.json", "src/config.json", 0, 0);
    expect_ignore("rule: an inner slash anchors", "src/*.tmp", "src/a.tmp", 0, 1);
    expect_ignore("rule: an inner slash anchors at root only", "src/*.tmp", "x/src/a.tmp", 0, 0);
    expect_ignore("rule: star does not span directories", "src/*.tmp", "src/sub/a.tmp", 0, 0);
    expect_ignore("rule: last matching line wins", "*.log\n!keep.log", "keep.log", 0, 0);
    expect_ignore("rule: negation order matters", "!keep.log\n*.log", "keep.log", 0, 1);
    expect_ignore("rule: comments are skipped", "# *.log\nreal", "a.log", 0, 0);
    expect_ignore("rule: blank lines are skipped", "\n\n*.log\n", "a.log", 0, 1);
    expect_ignore("rule: escaped hash is literal", "\\#y", "#y", 0, 1);
    expect_ignore("rule: escaped bang is literal", "\\!x", "!x", 0, 1);
    expect_ignore("rule: CRLF line endings", "*.log\r\nbuild/\r\n", "a.log", 0, 1);
    expect_ignore("rule: a bare star matches anything", "*", "anything", 0, 1);
    expect_ignore("rule: trailing-globstar also covers the directory itself",
                  "foo/**", "foo", 1, 1);

    /* D3: git skips a UTF-8 BOM at the start of .gitignore; gtree fed the
     * BOM into the first pattern, so the first rule in any .gitignore
     * saved by Notepad or PowerShell's Out-File silently did nothing. */
    expect_ignore("D3: a UTF-8 BOM does not disable the first pattern",
                  "\xEF\xBB\xBF*.log", "a.log", 0, 1);
    expect_ignore("D3: a BOM does not disable an anchored first pattern",
                  "\xEF\xBB\xBF/dist", "dist", 1, 1);
    expect_ignore("BOM does not affect later patterns", "\xEF\xBB\xBF*.log\nbuild/", "build", 1, 1);

    /* D7: POSIX bracket expressions. git's wildmatch implements them;
     * gtree read "[[:digit:]]" as a class of the literal characters
     * "[:digt", which both failed to match a digit and matched names
     * nobody asked for. */
    expect_ignore("D7: POSIX class [[:digit:]] matches a digit",
                  "[[:digit:]].c", "1.c", 0, 1);
    expect_ignore("D7: POSIX class [[:digit:]] rejects a letter",
                  "[[:digit:]].c", "a.c", 0, 0);
    expect_ignore("D7: POSIX class does not match its own punctuation",
                  "[[:digit:]].c", "].c", 0, 0);
    expect_ignore("D7: POSIX class [[:alpha:]] matches a letter",
                  "[[:alpha:]].c", "q.c", 0, 1);
    expect_ignore("POSIX class [[:upper:]] rejects lower case", "[[:upper:]].c", "q.c", 0, 0);
    expect_ignore("D7: POSIX class [[:upper:]] matches upper case",
                  "[[:upper:]].c", "Q.c", 0, 1);
    expect_ignore("D7: a POSIX class beside a literal in one class",
                  "[[:digit:]x].c", "x.c", 0, 1);
}

/* ------------------------------------------------------------------ */
/* The nested chain                                                    */
/* ------------------------------------------------------------------ */

static void chain_cases(void) {
    IgnoreChain *chain;
    IgnoreList *root;
    int saved_fe;

    MKDIR(TMPDIR "/frontend");
    MKDIR(TMPDIR "/frontend/src");
    MKDIR(TMPDIR "/backend");

    write_text(TMPDIR "/root.gitignore", "*.log\nnode_modules/\n/dist\n");
    write_text(TMPDIR "/frontend/.gitignore", "!debug.log\n*.map\n");
    write_text(TMPDIR "/backend/.gitignore", "secret.txt\n");

    root = ignore_list_load(TMPDIR "/root.gitignore");
    chain = ignore_chain_new(TMPDIR, root);
    if (!chain) { fprintf(stderr, "out of memory\n"); exit(2); }

    check_int("chain: root rule applies at the root", 1,
              ignore_chain_is_ignored(chain, "app.log", 0));
    check_int("chain: root rule applies at depth", 1,
              ignore_chain_is_ignored(chain, "frontend/app.log", 0));
    check_int("chain: nothing else is ignored yet", 0,
              ignore_chain_is_ignored(chain, "frontend/a.map", 0));

    saved_fe = ignore_chain_enter(chain, "frontend");
    check_int("chain: entering frontend found its .gitignore", 1, chain->total_nested_hit);
    check_int("chain: a nested rule applies inside its own directory", 1,
              ignore_chain_is_ignored(chain, "frontend/a.map", 0));
    check_int("chain: a nested rule applies further down", 1,
              ignore_chain_is_ignored(chain, "frontend/src/a.map", 0));

    /* D6: git gives the deepest .gitignore the last word, so
     * frontend/.gitignore's "!debug.log" re-includes a file that the root
     * .gitignore's "*.log" excluded. gtree checked levels root-first and
     * stopped at the first one that ignored the path, so a negation in a
     * nested file could never take effect. */
    check_int("D6: a nested negation overrides a root rule", 0,
              ignore_chain_is_ignored(chain, "frontend/debug.log", 0));
    check_int("D6: a nested negation applies below its directory too", 0,
              ignore_chain_is_ignored(chain, "frontend/src/debug.log", 0));

    check_int("chain: a nested rule does not leak upwards", 0,
              ignore_chain_is_ignored(chain, "a.map", 0));
    check_int("chain: a nested rule does not leak sideways", 0,
              ignore_chain_is_ignored(chain, "backend/a.map", 0));
    check_int("chain: a root rule still applies inside a nested scope", 1,
              ignore_chain_is_ignored(chain, "frontend/node_modules", 1));

    ignore_chain_leave(chain, saved_fe);
    check_int("chain: leaving frontend drops its rules", 0,
              ignore_chain_is_ignored(chain, "frontend/a.map", 0));
    check_int("chain: the hit counter does not decrease on leave", 1, chain->total_nested_hit);

    ignore_chain_enter(chain, "backend");
    check_int("chain: a second nested file is found", 2, chain->total_nested_hit);
    check_int("chain: backend's own rule applies", 1,
              ignore_chain_is_ignored(chain, "backend/secret.txt", 0));
    check_int("chain: backend's rule applies at depth", 1,
              ignore_chain_is_ignored(chain, "backend/src/secret.txt", 0));

    /* A directory whose name merely starts with a scope's name must not
     * pick up that scope's rules. */
    ignore_chain_enter(chain, "backendish");
    check_int("chain: a prefix-sharing sibling is not in scope", 0,
              ignore_chain_is_ignored(chain, "backendish/secret.txt", 0));

    ignore_chain_free(chain);
}

/* Entering a directory with no .gitignore, and a chain with no root dir
 * at all, must both be harmless. */
static void chain_absent_cases(void) {
    IgnoreList *root = ignore_list_load(TMPDIR "/root.gitignore");
    IgnoreChain *chain = ignore_chain_new(NULL, root);
    if (!chain) { fprintf(stderr, "out of memory\n"); exit(2); }
    /* enter() returns the level count as it was before the call, so a
     * fresh chain answers 1 whether or not it pushed anything. */
    check_int("chain: no filesystem root still reports the previous depth", 1,
              ignore_chain_enter(chain, "frontend"));
    check_int("chain: no nested files were found", 0, chain->total_nested_hit);
    check_int("chain: root rules still work", 1, ignore_chain_is_ignored(chain, "x.log", 0));
    ignore_chain_free(chain);

    root = ignore_list_load(TMPDIR "/root.gitignore");
    chain = ignore_chain_new(TMPDIR, root);
    if (!chain) { fprintf(stderr, "out of memory\n"); exit(2); }
    ignore_chain_enter(chain, "no-such-directory");
    check_int("chain: a directory with no .gitignore adds no level", 0, chain->total_nested_hit);
    ignore_chain_free(chain);
}

/* .git/info/exclude is a repository's private ignore file, in exactly the
 * .gitignore format, at a known path under the root. git applies it with
 * lower precedence than any .gitignore, so a .gitignore can override it. */
static void info_exclude_cases(void) {
    IgnoreList *root;
    IgnoreChain *chain;

    MKDIR(TMPDIR "/repo");
    MKDIR(TMPDIR "/repo/.git");
    MKDIR(TMPDIR "/repo/.git/info");
    MKDIR(TMPDIR "/repo/sub");
    write_text(TMPDIR "/repo/.git/info/exclude", "*.scratch\nprivate/\n!kept.scratch\n");
    write_text(TMPDIR "/repo/root.gitignore", "*.log\n!hidden.scratch\n");
    write_text(TMPDIR "/repo/sub/.gitignore", "*.scratch\n");

    root = ignore_list_load(TMPDIR "/repo/root.gitignore");
    chain = ignore_chain_new(TMPDIR "/repo", root);
    if (!chain) { fprintf(stderr, "out of memory\n"); exit(2); }

    check_int("D11: a rule in .git/info/exclude is applied", 1,
              ignore_chain_is_ignored(chain, "notes.scratch", 0));
    check_int("D11: a directory rule in .git/info/exclude is applied", 1,
              ignore_chain_is_ignored(chain, "private", 1));
    check_int("exclude: its own negation works", 0,
              ignore_chain_is_ignored(chain, "kept.scratch", 0));
    check_int("exclude: the root .gitignore still works", 1,
              ignore_chain_is_ignored(chain, "a.log", 0));

    /* Precedence: the root .gitignore is asked before .git/info/exclude, so
     * a negation there wins over an exclusion here. */
    check_int("exclude: a root .gitignore negation overrides it", 0,
              ignore_chain_is_ignored(chain, "hidden.scratch", 0));

    /* It is not counted as a nested per-directory file. */
    check_int("exclude: it is not counted among the nested files", 0, chain->total_nested_hit);
    ignore_chain_enter(chain, "sub");
    check_int("exclude: a real nested file still is", 1, chain->total_nested_hit);
    ignore_chain_free(chain);

    /* Without a filesystem root there is nowhere to look for it. */
    root = ignore_list_load(TMPDIR "/repo/root.gitignore");
    chain = ignore_chain_new(NULL, root);
    if (!chain) { fprintf(stderr, "out of memory\n"); exit(2); }
    check_int("exclude: no root directory means it is not read", 0,
              ignore_chain_is_ignored(chain, "notes.scratch", 0));
    ignore_chain_free(chain);
}

/* A root that already ends in a separator must not get a second one:
 * "//.gitignore" is the start of a UNC path on Windows, not a file at the
 * filesystem root, and the lookup goes looking for a machine on the network. */
static void separator_cases(void) {
    IgnoreList *root = ignore_list_load(TMPDIR "/repo/root.gitignore");
    IgnoreChain *chain = ignore_chain_new(TMPDIR "/repo/", root);
    if (!chain) { fprintf(stderr, "out of memory\n"); exit(2); }
    check_int("separator: a trailing slash on the root is not doubled", 1,
              ignore_chain_enter(chain, "sub") >= 1 ? chain->total_nested_hit : -1);
    ignore_chain_free(chain);
}

/* A missing file must read as an empty list rather than an error, because
 * "no .gitignore" and "an empty .gitignore" mean the same thing. */
static void absent_file_cases(void) {
    IgnoreList *l = ignore_list_load(TMPDIR "/definitely-not-here");
    check("load: a missing file yields an empty list", l != NULL);
    if (l) {
        check_int("load: the empty list has no patterns", 0, l->count);
        check_int("load: the empty list ignores nothing", 0, ignore_list_is_ignored(l, "x", 0));
        ignore_list_free(l);
    }
    l = ignore_list_load(NULL);
    check("load: a NULL path yields an empty list", l != NULL);
    ignore_list_free(l);
}

int main(void) {
    h_init("gitignore");
    MKDIR(TMPDIR);
    glob_cases();
    chain_cases();
    chain_absent_cases();
    info_exclude_cases();
    separator_cases();
    absent_file_cases();
    return h_report();
}
