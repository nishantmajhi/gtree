#ifndef GTREE_TREEIO_H
#define GTREE_TREEIO_H

#include <stdio.h>

/* The line-art style detected in / used for the tree text. */
typedef enum {
    STYLE_WIN_ASCII = 0, /* Windows "tree /F":      +---  \---  |    */
    STYLE_UNIX_UTF8,     /* Linux/macOS "tree":      ├──  └──  │     */
    STYLE_UNIX_ASCII,    /* Linux/macOS "tree -A":   |--  `--  |     */
    STYLE_UNKNOWN
} TreeStyle;

typedef struct TreeNode {
    char *name;                 /* display name, markers stripped     */
    int   is_dir;                /* best-effort directory guess        */
    struct TreeNode *parent;
    struct TreeNode *first_child;
    struct TreeNode *last_child; /* O(1) append                       */
    struct TreeNode *next_sibling;
    int   keep;                  /* set during gitignore filtering     */
} TreeNode;

/* Parses `tree`/`tree /F` style text from `in` into an in-memory tree.
 * Returns the synthetic root node (its own name is the first line of
 * input, e.g. "C:." or "."). Returns NULL only on allocation failure or
 * if the input contained no usable root line.
 * *detected_style is set to the line-art style seen most often.
 * If verbose is non-zero, unparsable lines are reported to stderr. */
TreeNode *tree_parse(FILE *in, TreeStyle *detected_style, int verbose);

/* Frees an entire tree. */
void tree_free(TreeNode *root);

/* Marks every node's `keep` flag according to the ignore chain (see
 * gitignore.h), skipping the ".git" directory unconditionally (matching
 * git's own behavior). As it descends into each directory, it looks for
 * a nested .gitignore on disk (via ignore_chain_enter) and applies it to
 * that subtree, same as a real `git` checkout. Counts of kept
 * directories/files are written to *dirs and *files. */
#include "gitignore.h"
void tree_filter(TreeNode *root, IgnoreChain *chain, int *dirs, int *files);

/* Renders the tree (only nodes with keep==1) to `out` using the given
 * line-art style. */
void tree_render(FILE *out, TreeNode *root, TreeStyle style);

#endif /* GTREE_TREEIO_H */
