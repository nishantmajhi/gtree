#ifndef GTREE_GITIGNORE_H
#define GTREE_GITIGNORE_H

/*
 * gitignore.h
 *
 * A small, dependency-free .gitignore pattern engine.
 *
 * Supports the subset of .gitignore syntax that covers the vast majority
 * of real-world files:
 *   - blank lines and lines starting with '#' are ignored (comments)
 *   - a leading '!' negates a pattern (re-includes a previously ignored path)
 *   - a trailing '/' restricts the pattern to directories only
 *   - a leading '/' (or any '/' that is not the last character) anchors
 *     the pattern to the root of the tree, otherwise the pattern is
 *     matched against the "basename" at any depth (git's normal rule)
 *   - glob wildcards '*' (any run of characters except '/'), '?'
 *     (single character except '/'), '[...]' character classes, and
 *     '**' (any run of characters, including '/')
 *   - later patterns override earlier ones, exactly like git
 *
 * NOTE: the input to this tool is the *text* output of a `tree` command
 * rather than a real filesystem, so gtree cannot discover a project's
 * directory structure on its own. But when a --root DIR is given (or
 * inferred from -g's path), gtree treats DIR as the filesystem location
 * that corresponds to the tree's root and, while walking the parsed
 * tree, looks for a `.gitignore` on disk in every subdirectory it
 * visits (DIR/<relpath>/.gitignore). Each one found is layered on top
 * of its ancestors' rules, the same way `git` itself applies nested
 * .gitignore files. See IgnoreChain below.
 */

typedef struct {
    char *raw;       /* original line, trimmed, for diagnostics             */
    char *pattern;   /* normalized pattern used for matching                */
    int   negate;    /* 1 if the line started with '!'                     */
    int   dir_only;  /* 1 if the line ended with '/'                       */
    int   anchored;  /* 1 if the pattern must match from the tree root     */
} IgnorePattern;

typedef struct {
    IgnorePattern *items;
    int count;
    int capacity;
} IgnoreList;

/* Loads and parses a .gitignore-style file. Returns an empty (non-NULL)
 * list if the file does not exist, so callers can treat "no .gitignore"
 * the same as "empty .gitignore". Returns NULL only on allocation failure. */
IgnoreList *ignore_list_load(const char *path);

/* Frees a list returned by ignore_list_load(). */
void ignore_list_free(IgnoreList *list);

/* Returns 1 if rel_path (posix-style, no leading '/', e.g. "src/foo.c")
 * should be ignored according to the loaded patterns, 0 otherwise.
 * is_dir must be 1 for directory entries and 0 for files. */
int ignore_list_is_ignored(const IgnoreList *list, const char *rel_path, int is_dir);

/* Exposed for unit testing: a standalone gitignore-flavored glob matcher.
 * Matches the whole of `text` against the whole of `pattern` (no partial
 * matches), where '*' does not cross '/', '**' does, and '?' / '[...]'
 * behave as in shell globs. */
int gtree_glob_match(const char *pattern, const char *text);

/* ------------------------------------------------------------------ */
/* Nested / per-directory .gitignore support                           */
/* ------------------------------------------------------------------ */

/* One level of the chain: the patterns from a single .gitignore file,
 * plus the directory (relative to the tree root, posix-style, '' for
 * the root itself, no trailing slash) that they apply to. */
typedef struct {
    char *base_rel_dir;
    IgnoreList *list;
} IgnoreChainLevel;

/* An ordered (root-to-leaf) stack of .gitignore levels, plus the real
 * filesystem directory that the tree's root corresponds to, so deeper
 * .gitignore files can be located on disk as the tree is walked. */
typedef struct {
    IgnoreChainLevel *levels;
    int count;
    int capacity;
    char *root_dir;       /* may be NULL if no filesystem root is known */
    int total_nested_hit; /* running count of nested .gitignore files found
                            * on disk over the chain's lifetime (doesn't
                            * decrease when ignore_chain_leave() pops levels
                            * back off); handy for --verbose reporting */
    int dirs_looked_for;  /* directories below the root that the tree walk
                            * went into                                    */
    int dirs_not_on_disk; /* how many of those are not there. Any at all
                            * means root_dir does not describe the listing,
                            * so nested .gitignore files were looked for in
                            * places that do not exist and never found --
                            * silently, until somebody reports this.       */
} IgnoreChain;

/* Creates a chain seeded with the root-level list (base_rel_dir == "").
 * `root_list` ownership transfers to the chain (freed by ignore_chain_free).
 * `root_dir` may be NULL, in which case ignore_chain_enter() never finds
 * any nested .gitignore files and the chain behaves like a single flat
 * list, same as before. */
IgnoreChain *ignore_chain_new(const char *root_dir, IgnoreList *root_list);

/* Frees the chain, including every level's IgnoreList. */
void ignore_chain_free(IgnoreChain *chain);

/* Call when descending into directory `rel_dir` (relative to the tree
 * root, posix-style, no leading '/'). If chain->root_dir is set and
 * `<root_dir>/<rel_dir>/.gitignore` exists on disk, it is loaded and
 * pushed onto the chain. Returns the chain's level count *before* this
 * call, so the caller can pass it to ignore_chain_leave() to pop back
 * off after finishing that subtree (whether or not a level was pushed). */
int ignore_chain_enter(IgnoreChain *chain, const char *rel_dir);

/* Pops the chain back down to `saved_count` levels (as returned by a
 * prior ignore_chain_enter()), freeing anything above it. */
void ignore_chain_leave(IgnoreChain *chain, int saved_count);

/* Returns 1 if rel_path (posix-style, no leading '/') should be ignored
 * according to the levels currently on the chain, 0 otherwise.
 *
 * Levels are consulted leaf-to-root and the deepest one that says anything
 * about the path decides, negation included -- which is git's rule, and is
 * what makes a "!debug.log" in src/.gitignore re-include a file that the
 * root .gitignore's "*.log" excluded. A level whose patterns do not mention
 * the path at all is passed over rather than treated as a decision, which
 * is why the engine needs a three-way verdict internally.
 *
 * The one rule this looks like it breaks -- that nothing inside an ignored
 * *directory* can be re-included -- is enforced by the caller instead: the
 * tree walk does not descend into a directory it has pruned, so no path
 * below one is ever asked about. git reaches the same outcome the same way. */
int ignore_chain_is_ignored(const IgnoreChain *chain, const char *rel_path, int is_dir);

#endif /* GTREE_GITIGNORE_H */
