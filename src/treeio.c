#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "treeio.h"
#include "gitignore.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static void rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static char *ltrim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int stristr_has(const char *hay, const char *needle) {
    size_t hn = strlen(hay), nn = strlen(needle), i;
    if (nn == 0 || hn < nn) return 0;
    for (i = 0; i + nn <= hn; i++) {
        size_t j;
        int ok = 1;
        for (j = 0; j < nn; j++) {
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static char *read_line_alloc(FILE *f) {
    size_t cap = 128, len = 0;
    char *buf = (char *)malloc(cap);
    int c;
    int got_any = 0;

    if (!buf) return NULL;

    while ((c = fgetc(f)) != EOF) {
        got_any = 1;
        if (len + 1 >= cap) {
            size_t new_cap = cap * 2;
            char *grown = (char *)realloc(buf, new_cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
            cap = new_cap;
        }
        if (c == '\n') break;
        buf[len++] = (char)c;
    }

    if (!got_any) { free(buf); return NULL; }

    buf[len] = '\0';
    rtrim(buf);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Line-art group recognition                                          */
/* ------------------------------------------------------------------ */

typedef enum { G_CONT_LINE, G_CONT_BLANK, G_CONN_MID, G_CONN_LAST, G_NONE } GroupKind;

typedef struct {
    const char *bytes;
    int len;
    GroupKind kind;
    TreeStyle style; /* only meaningful for G_CONN_MID / G_CONN_LAST */
} GroupDef;

static const GroupDef GROUPS[] = {
    /* Windows "tree /F" */
    { "+---", 4, G_CONN_MID,  STYLE_WIN_ASCII },
    { "\\---", 4, G_CONN_LAST, STYLE_WIN_ASCII },
    /* Windows "tree /F" continuations. The vertical bar is not ASCII '|'
     * unless /A was given: a redirected `tree /F` writes the single byte
     * 0xA6, and a console in codepage 437 or 850 uses 0xB3 for U+2502.
     * Both appear in the wild, neither is valid UTF-8, and without them
     * every file line in the default dialect is unparseable -- which is
     * most of the output, because `tree /F` gives files no connector at
     * all and indents them with continuations only. */
    { "\xA6   ", 4, G_CONT_LINE, STYLE_WIN_ASCII },
    { "\xB3   ", 4, G_CONT_LINE, STYLE_WIN_ASCII },
    /* Linux/macOS "tree" (UTF-8 box drawing).
     * "├── " = E2 94 9C (├) + E2 94 80 (─) + E2 94 80 (─) + ' ' = 10 bytes.
     * "└── " likewise = 10 bytes. "│   " = E2 94 82 (│) + 3 spaces = 6 bytes. */
    { "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ", 10, G_CONN_MID,  STYLE_UNIX_UTF8 },
    { "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ", 10, G_CONN_LAST, STYLE_UNIX_UTF8 },
    { "\xE2\x94\x82   ",                        6, G_CONT_LINE, STYLE_UNIX_UTF8 },
    /* Linux/macOS "tree -A" (ASCII fallback) */
    { "|-- ", 4, G_CONN_MID,  STYLE_UNIX_ASCII },
    { "`-- ", 4, G_CONN_LAST, STYLE_UNIX_ASCII },
    /* Shared continuation used by Windows and "tree -A" */
    { "|   ", 4, G_CONT_LINE, STYLE_WIN_ASCII },
    /* Generic blank continuation (four columns, no vertical bar) */
    { "    ", 4, G_CONT_BLANK, STYLE_UNKNOWN },
};
#define N_GROUPS (int)(sizeof(GROUPS) / sizeof(GROUPS[0]))

static const GroupDef *match_group(const char *s) {
    int i;
    for (i = 0; i < N_GROUPS; i++) {
        int len = GROUPS[i].len;
        if (strncmp(s, GROUPS[i].bytes, (size_t)len) == 0) return &GROUPS[i];
    }
    return NULL;
}

/* Every spelling of the vertical bar that appears in tree output: ASCII,
 * the two single-byte OEM forms, and UTF-8 U+2502. */
static int vertical_bar_len(const char *s) {
    if (*s == '|' || *s == '\xA6' || *s == '\xB3') return 1;
    if ((unsigned char)s[0] == 0xE2 && (unsigned char)s[1] == 0x94 &&
        (unsigned char)s[2] == 0x82) return 3;
    return 0;
}

/* Windows `tree /F` writes a line of pure indentation between a directory's
 * files and its subdirectories:
 *
 *     |   README.md
 *     |               <- this one
 *     +---build
 *
 * Those trailing spaces do not survive being trimmed, which left a bare
 * vertical bar behind. The bar is not an entry name: it used to become a
 * directory called "|", and because it sat at the depth the next line
 * expected its parent to be, the whole rest of the tree was re-parented
 * under it. Every relative path then carried an extra segment, so ignore
 * rules stopped matching and nested .gitignore files were looked for in
 * directories that do not exist.
 *
 * Returns 1 when what is left of a line after its indentation is bars and
 * whitespace, and so carries no entry. */
static int is_spacer_remainder(const char *s) {
    while (*s) {
        int bar = vertical_bar_len(s);
        if (bar) { s += bar; continue; }
        if (*s == ' ' || *s == '\t') { s++; continue; }
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Header / footer detection (lines that are not tree entries)        */
/* ------------------------------------------------------------------ */

static int looks_like_header(const char *line) {
    return stristr_has(line, "path listing") ||
           stristr_has(line, "serial number") ||
           stristr_has(line, "volume in drive");
}

static int looks_like_summary(const char *line) {
    /* GNU/BSD tree prints e.g. "8 directories, 15 files" as a footer.
     * Windows tree /F prints no such footer, so this only ever fires
     * for Unix-style input. */
    return stristr_has(line, "director") && stristr_has(line, "file");
}

/* ------------------------------------------------------------------ */
/* Tree node helpers                                                   */
/* ------------------------------------------------------------------ */

static TreeNode *node_new(const char *name, int is_dir) {
    TreeNode *n = (TreeNode *)calloc(1, sizeof(TreeNode));
    if (!n) return NULL;
    n->name = xstrdup(name);
    n->is_dir = is_dir;
    n->keep = 1;
    return n;
}

static void node_append_child(TreeNode *parent, TreeNode *child) {
    child->parent = parent;
    if (parent->last_child) {
        parent->last_child->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
    parent->is_dir = 1; /* having a child proves this is a directory */
}

void tree_free(TreeNode *root) {
    TreeNode *child, *next;
    if (!root) return;
    child = root->first_child;
    while (child) {
        next = child->next_sibling;
        tree_free(child);
        child = next;
    }
    free(root->name);
    free(root);
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

#define INITIAL_STACK_CAP 64

TreeNode *tree_parse(FILE *in, TreeStyle *detected_style, int verbose) {
    TreeNode *root = NULL;
    TreeNode **stack;
    int stack_cap = INITIAL_STACK_CAP;
    char *line;
    int win_count = 0, u8_count = 0, uascii_count = 0;
    /* The most recent line seen before any entry: the root candidate. See
     * the depth == 0 branch below for why it is the last one and not the
     * first. */
    char *pending_root = NULL;

    stack = (TreeNode **)calloc((size_t)stack_cap, sizeof(TreeNode *));
    if (!stack) return NULL;

    while ((line = read_line_alloc(in)) != NULL) {
        const char *p = line;
        int depth = 0;
        TreeStyle last_style = STYLE_UNKNOWN;
        char *name;
        size_t nlen;
        int is_dir = 0;

        if (line[0] == '\0') { free(line); continue; }

        for (;;) {
            const GroupDef *g = match_group(p);
            if (!g) break;
            p += g->len;
            depth++;
            if (g->kind == G_CONN_MID || g->kind == G_CONN_LAST) {
                last_style = g->style;
                if (last_style == STYLE_WIN_ASCII) win_count++;
                else if (last_style == STYLE_UNIX_UTF8) u8_count++;
                else if (last_style == STYLE_UNIX_ASCII) uascii_count++;
                break;
            }
        }

        if (depth == 0) {
            /* A line with no indentation, before any entry, is the root --
             * but `tree /F` prints one or two header lines above it, so it
             * is the *last* such line that is the root, not the first. The
             * two header phrases are still screened out, but relying on
             * them alone was a bug: a Windows whose tree speaks German
             * prints "Auflistung der Ordnerpfade", which matches nothing
             * here, so the header became the tree's root and the real root
             * line was discarded as a duplicate. Taking the last candidate
             * needs no list of phrases in any language. */
            if (root == NULL && !looks_like_header(line) && !looks_like_summary(line)) {
                free(pending_root);
                pending_root = xstrdup(ltrim(line));
            }
            free(line);
            continue;
        }

        if (root == NULL) {
            if (!pending_root) {
                /* Indented lines with nothing above them: no tree. */
                if (verbose) fprintf(stderr, "gtree: skipping line before root: %s\n", line);
                free(line);
                continue;
            }
            root = node_new(pending_root, 1);
            if (!root) { free(line); free(pending_root); free(stack); return NULL; }
            stack[0] = root;
        }

        if (depth >= stack_cap) {
            int new_cap = stack_cap;
            TreeNode **grown;
            while (depth >= new_cap) new_cap *= 2;
            grown = (TreeNode **)realloc(stack, (size_t)new_cap * sizeof(TreeNode *));
            if (!grown) { free(line); free(pending_root); free(stack); return root; }
            stack = grown;
            stack_cap = new_cap;
        }

        /* A line with indentation and no connector is not a malformed line:
         * it is how Windows `tree /F` writes a file. Only the directories
         * get "+---" or "\---"; a file is indented one group further than
         * its directory and named directly. Warning about it was wrong
         * enough to be actively misleading, since it fires for most of the
         * lines in the program's headline input. */

        if (is_spacer_remainder(p)) { free(line); continue; }

        name = ltrim((char *)p);
        rtrim(name);
        nlen = strlen(name);
        if (nlen == 0) { free(line); continue; }

        {
            char last_ch = name[nlen - 1];
            if (last_ch == '/' ) {
                is_dir = 1;
                name[nlen - 1] = '\0';
            } else if (last_ch == '*' || last_ch == '@' || last_ch == '=' || last_ch == '>' ) {
                /* `tree -F` classifier for executables/symlinks/sockets/
                 * doors; strip it, but it doesn't imply a directory. */
                name[nlen - 1] = '\0';
            }
        }

        if (!stack[depth - 1]) {
            if (verbose) fprintf(stderr, "gtree: no parent at depth %d for: %s\n", depth, name);
            free(line);
            continue;
        }

        {
            TreeNode *node = node_new(name, is_dir);
            if (!node) { free(line); free(pending_root); free(stack); return root; }
            node_append_child(stack[depth - 1], node);
            if (is_dir) node->is_dir = 1; /* keep explicit marker even before any child is seen */
            stack[depth] = node;
        }

        free(line);
    }

    free(stack);

    /* Input that was nothing but a root line: no entry ever arrived to
     * promote the candidate, so do it here. */
    if (root == NULL && pending_root != NULL) {
        root = node_new(pending_root, 1);
    }
    free(pending_root);

    if (detected_style) {
        if (u8_count >= win_count && u8_count >= uascii_count && u8_count > 0) {
            *detected_style = STYLE_UNIX_UTF8;
        } else if (uascii_count >= win_count && uascii_count > 0) {
            *detected_style = STYLE_UNIX_ASCII;
        } else {
            *detected_style = STYLE_WIN_ASCII;
        }
    }

    return root;
}

/* ------------------------------------------------------------------ */
/* Filtering                                                           */
/* ------------------------------------------------------------------ */

/* Clears keep on a node and everything below it. */
static void mark_subtree_dropped(TreeNode *node) {
    TreeNode *c;
    node->keep = 0;
    for (c = node->first_child; c; c = c->next_sibling) mark_subtree_dropped(c);
}

static void filter_recursive(TreeNode *node, IgnoreChain *chain, const char *rel_prefix,
                              int *dirs, int *files) {
    TreeNode *child;
    for (child = node->first_child; child; child = child->next_sibling) {
        char *rel;
        size_t plen = strlen(rel_prefix);
        size_t nlen = strlen(child->name);

        if (strcmp(child->name, ".git") == 0) {
            mark_subtree_dropped(child);
            continue;
        }

        rel = (char *)malloc(plen + 1 + nlen + 1);
        if (!rel) { mark_subtree_dropped(child); continue; }
        if (plen == 0) {
            memcpy(rel, child->name, nlen + 1);
        } else {
            memcpy(rel, rel_prefix, plen);
            rel[plen] = '/';
            memcpy(rel + plen + 1, child->name, nlen + 1);
        }

        if (ignore_chain_is_ignored(chain, rel, child->is_dir)) {
            /* Clear the whole subtree, not just this node. Rendering never
             * reached a pruned directory's children so it did not matter to
             * the output, but it left every one of them with keep == 1 --
             * a flag that says "this survived filtering" on a node that did
             * not. Anything later that consults it, a count or a report of
             * what was hidden, would read it and be wrong. */
            mark_subtree_dropped(child);
        } else {
            child->keep = 1;
            if (child->is_dir) { if (dirs) (*dirs)++; }
            else { if (files) (*files)++; }

            if (child->is_dir) {
                /* Look for a nested .gitignore in this directory on disk
                 * (if we know the filesystem root) before descending, and
                 * pop it back off once this subtree is done. */
                int saved = ignore_chain_enter(chain, rel);
                filter_recursive(child, chain, rel, dirs, files);
                ignore_chain_leave(chain, saved);
            } else {
                filter_recursive(child, chain, rel, dirs, files);
            }
        }

        free(rel);
    }
}

void tree_filter(TreeNode *root, IgnoreChain *chain, int *dirs, int *files) {
    if (dirs) *dirs = 0;
    if (files) *files = 0;
    if (!root) return;
    root->keep = 1;
    filter_recursive(root, chain, "", dirs, files);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *conn_mid;
    const char *conn_last;
    const char *cont_line;
    const char *cont_blank;
} StyleChars;

static const StyleChars *style_chars(TreeStyle style) {
    static const StyleChars win  = { "+---", "\\---", "|   ", "    " };
    static const StyleChars u8   = { "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ",
                                      "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ",
                                      "\xE2\x94\x82   ", "    " };
    static const StyleChars ua   = { "|-- ", "`-- ", "|   ", "    " };
    switch (style) {
        case STYLE_UNIX_UTF8:  return &u8;
        case STYLE_UNIX_ASCII: return &ua;
        default:                return &win;
    }
}

static int has_kept_children(const TreeNode *node) {
    const TreeNode *c;
    for (c = node->first_child; c; c = c->next_sibling) {
        if (c->keep) return 1;
    }
    return 0;
}

static void render_children(FILE *out, const TreeNode *node, const char *prefix, const StyleChars *sc) {
    const TreeNode *c;
    const TreeNode *last_kept = NULL;

    for (c = node->first_child; c; c = c->next_sibling) {
        if (c->keep) last_kept = c;
    }

    for (c = node->first_child; c; c = c->next_sibling) {
        int is_last;
        char *new_prefix;
        size_t plen, extra_len;

        if (!c->keep) continue;
        is_last = (c == last_kept);

        fprintf(out, "%s%s%s%s\n", prefix, is_last ? sc->conn_last : sc->conn_mid,
                c->name, c->is_dir ? "" : "");

        if (!has_kept_children(c)) continue;

        plen = strlen(prefix);
        extra_len = strlen(is_last ? sc->cont_blank : sc->cont_line);
        new_prefix = (char *)malloc(plen + extra_len + 1);
        if (!new_prefix) continue;
        memcpy(new_prefix, prefix, plen);
        memcpy(new_prefix + plen, is_last ? sc->cont_blank : sc->cont_line, extra_len);
        new_prefix[plen + extra_len] = '\0';

        render_children(out, c, new_prefix, sc);
        free(new_prefix);
    }
}

void tree_render(FILE *out, TreeNode *root, TreeStyle style) {
    const StyleChars *sc = style_chars(style);
    if (!root) return;
    fprintf(out, "%s\n", root->name);
    render_children(out, root, "", sc);
}
