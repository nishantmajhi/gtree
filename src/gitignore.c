#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "gitignore.h"

/* ------------------------------------------------------------------ */
/* Small string helpers                                                */
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

/* ------------------------------------------------------------------ */
/* Glob matcher: '*' (no '/'), '**' (any incl '/'), '?', '[...]'       */
/* ------------------------------------------------------------------ */

/* ---- POSIX bracket expressions inside a character class ------------- */

/* If `p` points at the "[:name:]" of a POSIX class, returns its whole
 * length including both brackets, otherwise 0. git's matcher implements
 * these; gtree's read "[[:digit:]]" as a class of the literal characters
 * "[:digt" followed by the literal text "]", which meant it matched names
 * nobody had asked for (a file called "].c" for the pattern
 * "[[:digit:]].c") and failed to match the digits that were the point. */
static int posix_class_len(const char *p) {
    const char *q;
    if (p[0] != '[' || p[1] != ':') return 0;
    for (q = p + 2; *q && *q != ':'; q++) {
        if (!isalpha((unsigned char)*q)) return 0;
    }
    if (q[0] != ':' || q[1] != ']') return 0;
    return (int)(q + 2 - p);
}

/* Does `c` belong to the POSIX class named by `name` (of length `len`)?
 * An unknown class name matches nothing, which is what git does with it. */
static int posix_class_matches(const char *name, size_t len, char c) {
    unsigned char u = (unsigned char)c;
    struct { const char *name; int (*fn)(int); } table[] = {
        { "alnum",  isalnum  }, { "alpha",  isalpha  }, { "blank",  NULL     },
        { "cntrl",  iscntrl  }, { "digit",  isdigit  }, { "graph",  isgraph  },
        { "lower",  islower  }, { "print",  isprint  }, { "punct",  ispunct  },
        { "space",  isspace  }, { "upper",  isupper  }, { "xdigit", isxdigit }
    };
    size_t i;
    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strlen(table[i].name) == len && strncmp(table[i].name, name, len) == 0) {
            /* C89 has no isblank(), so spell that one out. */
            if (!table[i].fn) return u == ' ' || u == '\t';
            return table[i].fn(u) != 0;
        }
    }
    return 0;
}

static int match_here(const char *pat, const char *str);

static int match_star(int dotall, const char *pat, const char *str) {
    for (;;) {
        if (match_here(pat, str)) return 1;
        if (*str == '\0') return 0;
        if (!dotall && *str == '/') return 0;
        str++;
    }
}

static int match_here_uncached(const char *pat, const char *str) {
    if (*pat == '\0') return *str == '\0';

    if (pat[0] == '*' && pat[1] == '*' && pat[2] == '/') {
        /* git rule: a pattern like a-slash-doublestar-slash-b matches
         * "a/b", "a/x/b", "a/x/y/b", and so on: a doublestar bounded by
         * slashes on both sides matches zero or more whole directory
         * segments, together with the slash on each side. */
        const char *rest = pat + 3;
        const char *s = str;

        if (match_here(rest, str)) return 1; /* zero segments consumed */

        while (*s && *s != '/') s++;
        if (*s == '/') return match_here(pat, s + 1); /* consume one segment, keep trying */
        return 0;
    }

    if (*pat == '*') {
        int dotall = (pat[1] == '*');
        const char *p = pat + (dotall ? 2 : 1);
        while (*p == '*') p++; /* collapse "***" etc. */
        return match_star(dotall, p, str);
    }

    if (*pat == '?') {
        if (*str == '\0' || *str == '/') return 0;
        return match_here(pat + 1, str + 1);
    }

    if (*pat == '[') {
        const char *p = pat + 1;
        int neg = 0;
        int matched = 0;
        const char *start;

        if (*p == '!' || *p == '^') { neg = 1; p++; }
        start = p;

        while (*p && (*p != ']' || p == start)) {
            char c1;
            int klen = posix_class_len(p);
            if (klen > 0) {
                if (*str && posix_class_matches(p + 2, (size_t)(klen - 4), *str)) matched = 1;
                p += klen;
                continue;
            }
            c1 = *p++;
            if (*p == '-' && p[1] && p[1] != ']') {
                char c2 = p[1];
                if (*str && *str >= c1 && *str <= c2) matched = 1;
                p += 2;
            } else {
                if (*str == c1) matched = 1;
            }
        }
        if (*p != ']') return 0; /* malformed class: never matches */
        p++;

        if (*str == '\0' || *str == '/') return 0;
        if (neg) matched = !matched;
        if (!matched) return 0;
        return match_here(p, str + 1);
    }

    if (*pat == '\\' && pat[1] != '\0') {
        if (*str != pat[1]) return 0;
        return match_here(pat + 2, str + 1);
    }

    if (*str == '\0') return 0;
    if (*pat != *str) return 0;
    return match_here(pat + 1, str + 1);
}

/* ---- Making the matcher terminate ---------------------------------- */

/* match_here() is a plain backtracking matcher, and a pattern with several
 * stars in it makes that backtracking exponential: "*a*a*a*a*a*a*a*a*a*a*b"
 * against fifty a's followed by one wrong character does not come back
 * inside a human lifetime, and the only symptom is gtree appearing to hang
 * on somebody's .gitignore.
 *
 * The function is pure -- its answer depends on nothing but the two
 * pointers -- so remembering which (pattern position, text position) pairs
 * have already been shown to fail is enough to bound it at
 * O(pattern x text), and cannot change any answer. One bit per pair.
 *
 * The table is set up per top-level call, so recursion sees it through
 * these globals. That is safe here because gtree is a single-threaded
 * filter; a threaded caller would need the context passed down instead. */
#define MEMO_MAX_BITS (8u * 1024u * 1024u)   /* 1 MB, far beyond real input */

static struct {
    const char *pat0;
    const char *str0;
    size_t stride;          /* text length + 1 */
    unsigned char *seen;    /* NULL disables memoisation entirely */
} g_memo;

static int match_here(const char *pat, const char *str) {
    size_t idx;
    int r;

    if (!g_memo.seen) return match_here_uncached(pat, str);

    idx = (size_t)(pat - g_memo.pat0) * g_memo.stride + (size_t)(str - g_memo.str0);
    if (g_memo.seen[idx >> 3] & (unsigned char)(1u << (idx & 7u))) return 0;

    r = match_here_uncached(pat, str);
    if (!r) g_memo.seen[idx >> 3] |= (unsigned char)(1u << (idx & 7u));
    return r;
}

int gtree_glob_match(const char *pattern, const char *text) {
    size_t plen = strlen(pattern);
    size_t tlen = strlen(text);
    size_t bits = (plen + 1) * (tlen + 1);
    int result;

    g_memo.pat0 = pattern;
    g_memo.str0 = text;
    g_memo.stride = tlen + 1;
    g_memo.seen = NULL;

    /* Anything short cannot blow up, and anything enormous would want more
     * table than it is worth; both run uncached. */
    if (bits > 64 && bits <= MEMO_MAX_BITS) {
        g_memo.seen = (unsigned char *)calloc((bits + 7) / 8, 1);
    }

    result = match_here(pattern, text);

    free(g_memo.seen);
    g_memo.seen = NULL;
    return result;
}

/* The verdict of one .gitignore file on one path; see the definition. */
static int list_verdict(const IgnoreList *list, const char *rel_path, int is_dir);

/* Returns the last path component of a posix-style path. */
static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */
/* ------------------------------------------------------------------ */

static IgnoreList *ignore_list_new(void) {
    IgnoreList *list = (IgnoreList *)calloc(1, sizeof(IgnoreList));
    if (!list) return NULL;
    list->capacity = 16;
    list->items = (IgnorePattern *)calloc((size_t)list->capacity, sizeof(IgnorePattern));
    if (!list->items) { free(list); return NULL; }
    return list;
}

static int ignore_list_push(IgnoreList *list, IgnorePattern item) {
    if (list->count == list->capacity) {
        int new_cap = list->capacity * 2;
        IgnorePattern *grown = (IgnorePattern *)realloc(list->items, (size_t)new_cap * sizeof(IgnorePattern));
        if (!grown) return 0;
        list->items = grown;
        list->capacity = new_cap;
    }
    list->items[list->count++] = item;
    return 1;
}

/* Reads one line of arbitrary length from f into a malloc'd buffer.
 * Returns NULL at EOF (with nothing read). Strips the trailing newline. */
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

IgnoreList *ignore_list_load(const char *path) {
    IgnoreList *list = ignore_list_new();
    FILE *f;
    char *line;

    if (!list) return NULL;
    if (!path) return list;

    f = fopen(path, "rb");
    if (!f) return list; /* no .gitignore == empty list, not an error */

    /* A .gitignore written by Notepad, or by PowerShell's Out-File, or by
     * anything else that defaults to "UTF-8 with signature", starts with a
     * three-byte BOM. git skips it. gtree did not, so the BOM became part of
     * the first pattern and the first rule in the file silently did nothing
     * -- a failure with no symptom except a file you expected to be hidden
     * appearing in the listing. Only the first line can carry one. */
    {
        unsigned char bom[3];
        size_t got = fread(bom, 1, sizeof bom, f);
        if (!(got == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
            rewind(f);
        }
    }

    while ((line = read_line_alloc(f)) != NULL) {
        char *trimmed = ltrim(line);
        size_t len;
        int negate = 0, dir_only = 0, anchored = 0;
        char *pat;

        if (trimmed[0] == '\0' || trimmed[0] == '#') { free(line); continue; }

        if (trimmed[0] == '!') { negate = 1; trimmed++; }

        /* Unescape a leading "\!" or "\#" (literal, not special). */
        if (trimmed[0] == '\\' && (trimmed[1] == '!' || trimmed[1] == '#')) {
            trimmed++;
        }

        len = strlen(trimmed);
        if (len > 0 && trimmed[len - 1] == '/') {
            dir_only = 1;
            trimmed[len - 1] = '\0';
            len--;
        }
        if (len == 0) { free(line); continue; }

        if (trimmed[0] == '/') {
            anchored = 1;
            trimmed++;
        } else if (strchr(trimmed, '/') != NULL) {
            /* A '/' anywhere except as the very last char (already
             * stripped above) anchors the pattern, per gitignore rules. */
            anchored = 1;
        }

        pat = xstrdup(trimmed);
        if (!pat) { free(line); continue; }

        {
            IgnorePattern item;
            item.raw = xstrdup(line);
            item.pattern = pat;
            item.negate = negate;
            item.dir_only = dir_only;
            item.anchored = anchored;
            if (!ignore_list_push(list, item)) {
                free(item.raw);
                free(item.pattern);
            }
        }
        free(line);
    }

    fclose(f);
    return list;
}

void ignore_list_free(IgnoreList *list) {
    int i;
    if (!list) return;
    for (i = 0; i < list->count; i++) {
        free(list->items[i].raw);
        free(list->items[i].pattern);
    }
    free(list->items);
    free(list);
}

/* ------------------------------------------------------------------ */
/* Nested / per-directory .gitignore support (IgnoreChain)             */
/* ------------------------------------------------------------------ */

/* Defined below, next to the other filesystem helpers. */
static char *build_fs_path(const char *root_dir, const char *rel_dir, const char *leaf);
static int directory_exists(const char *root_dir, const char *rel_dir);

IgnoreChain *ignore_chain_new(const char *root_dir, IgnoreList *root_list) {
    IgnoreChain *chain = (IgnoreChain *)calloc(1, sizeof(IgnoreChain));
    if (!chain) return NULL;

    chain->capacity = 8;
    chain->levels = (IgnoreChainLevel *)calloc((size_t)chain->capacity, sizeof(IgnoreChainLevel));
    if (!chain->levels) { free(chain); return NULL; }

    chain->root_dir = root_dir ? xstrdup(root_dir) : NULL;

    /* .git/info/exclude holds a repository's private rules, in exactly the
     * .gitignore format, at a known path. git applies it with *lower*
     * precedence than any .gitignore, so it goes on the chain first: levels
     * are consulted leaf-to-root, which makes level 0 the last one asked.
     *
     * If it is absent, ignore_list_load returns an empty list, which costs
     * one level that decides nothing. It is not counted in
     * total_nested_hit, which reports per-directory .gitignore files. */
    if (chain->root_dir) {
        char *path = build_fs_path(chain->root_dir, "", ".git/info/exclude");
        if (path) {
            IgnoreList *excl = ignore_list_load(path);
            free(path);
            if (excl) {
                chain->levels[chain->count].base_rel_dir = xstrdup("");
                chain->levels[chain->count].list = excl;
                chain->count++;
            }
        }
    }

    chain->levels[chain->count].base_rel_dir = xstrdup("");
    chain->levels[chain->count].list = root_list;
    chain->count++;

    return chain;
}

void ignore_chain_free(IgnoreChain *chain) {
    int i;
    if (!chain) return;
    for (i = 0; i < chain->count; i++) {
        free(chain->levels[i].base_rel_dir);
        ignore_list_free(chain->levels[i].list);
    }
    free(chain->levels);
    free(chain->root_dir);
    free(chain);
}

/* Joins chain->root_dir and rel_dir into a filesystem path using '/'
 * (works fine for reading files on Windows too), then appends
 * "/.gitignore". Returns a malloc'd string, or NULL on OOM. */
/* Joins root_dir, an optional relative directory and a leaf name with '/'.
 * Forward slashes are fine for opening files on Windows as well, so one
 * separator serves both platforms; these paths are never displayed.
 *
 * A root that already ends in a separator does not get a second one. That
 * is not tidiness: "/" or "\" as the root would otherwise produce
 * "//.gitignore", which Windows reads as the start of a UNC path and looks
 * for a machine on the network called ".gitignore". A root of "\foo" is how
 * this arises in practice, because the directory part of "\foo\.gitignore"
 * is "\". */
static char *build_fs_path(const char *root_dir, const char *rel_dir, const char *leaf) {
    size_t rlen = strlen(root_dir);
    size_t dlen = strlen(rel_dir);
    size_t need = rlen + 1 + dlen + 1 + strlen(leaf) + 1;
    int rooted = (rlen > 0 && (root_dir[rlen - 1] == '/' || root_dir[rlen - 1] == '\\'));
    const char *sep = rooted ? "" : "/";
    char *out = (char *)malloc(need);
    if (!out) return NULL;

    if (dlen == 0) {
        snprintf(out, need, "%s%s%s", root_dir, sep, leaf);
    } else {
        snprintf(out, need, "%s%s%s/%s", root_dir, sep, rel_dir, leaf);
    }
    return out;
}

static char *build_gitignore_fs_path(const char *root_dir, const char *rel_dir) {
    return build_fs_path(root_dir, rel_dir, ".gitignore");
}

/* Is <root_dir>/<rel_dir> a directory on this filesystem? */
static int directory_exists(const char *root_dir, const char *rel_dir) {
    size_t rlen = strlen(root_dir);
    size_t dlen = strlen(rel_dir);
    int rooted = (rlen > 0 && (root_dir[rlen - 1] == '/' || root_dir[rlen - 1] == '\\'));
    size_t need = rlen + 1 + dlen + 1;
    char *path = (char *)malloc(need);
    struct stat st;
    int ok;

    if (!path) return 1; /* cannot tell; do not claim it is missing */
    snprintf(path, need, "%s%s%s", root_dir, rooted ? "" : "/", rel_dir);
    /* S_ISDIR, not S_IFMT/S_IFDIR: glibc hides those two under -std=c99. */
    ok = (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
    free(path);
    return ok;
}

int ignore_chain_enter(IgnoreChain *chain, const char *rel_dir) {
    int saved_count;
    char *fs_path;
    FILE *probe;

    if (!chain) return 0;
    saved_count = chain->count;

    if (!chain->root_dir) return saved_count;

    /* The tree text may have been captured on another machine, or before a
     * refactor, in which case --root points at a directory that no longer
     * matches it. Nested .gitignore files are then looked for in places that
     * do not exist and simply never found, so the output is quietly missing
     * rules with nothing to say so. Count the directories that are not there
     * and let the caller report it. */
    if (rel_dir[0] != '\0') {
        chain->dirs_looked_for++;
        if (!directory_exists(chain->root_dir, rel_dir)) {
            chain->dirs_not_on_disk++;
        }
    }

    fs_path = build_gitignore_fs_path(chain->root_dir, rel_dir);
    if (!fs_path) return saved_count;

    /* The root directory's own .gitignore is loaded by ignore_chain_new;
     * don't load it a second time. */
    if (rel_dir[0] == '\0') { free(fs_path); return saved_count; }

    probe = fopen(fs_path, "rb");
    if (!probe) { free(fs_path); return saved_count; }
    fclose(probe);

    {
        IgnoreList *nested = ignore_list_load(fs_path);
        free(fs_path);
        if (!nested) return saved_count; /* OOM: skip this level, keep going */

        if (chain->count == chain->capacity) {
            int new_cap = chain->capacity * 2;
            IgnoreChainLevel *grown =
                (IgnoreChainLevel *)realloc(chain->levels, (size_t)new_cap * sizeof(IgnoreChainLevel));
            if (!grown) { ignore_list_free(nested); return saved_count; }
            chain->levels = grown;
            chain->capacity = new_cap;
        }

        chain->levels[chain->count].base_rel_dir = xstrdup(rel_dir);
        chain->levels[chain->count].list = nested;
        chain->count++;
        chain->total_nested_hit++;
    }

    return saved_count;
}

void ignore_chain_leave(IgnoreChain *chain, int saved_count) {
    int i;
    if (!chain) return;
    for (i = saved_count; i < chain->count; i++) {
        free(chain->levels[i].base_rel_dir);
        ignore_list_free(chain->levels[i].list);
    }
    chain->count = saved_count;
}

/* Returns the portion of rel_path under base_rel_dir, or NULL if
 * rel_path is not inside base_rel_dir at all. Both are posix-style,
 * no leading/trailing slashes (base_rel_dir == "" means "everything"). */
static const char *rel_path_under(const char *base_rel_dir, const char *rel_path) {
    size_t blen = strlen(base_rel_dir);
    size_t plen;

    if (blen == 0) return rel_path;

    plen = strlen(rel_path);
    if (plen < blen) return NULL;
    if (strncmp(rel_path, base_rel_dir, blen) != 0) return NULL;
    if (rel_path[blen] == '\0') return NULL; /* the base dir itself, not something under it */
    if (rel_path[blen] != '/') return NULL;  /* e.g. base "src" must not match "src2/..." */

    return rel_path + blen + 1;
}

int ignore_chain_is_ignored(const IgnoreChain *chain, const char *rel_path, int is_dir) {
    int i;
    if (!chain) return 0;

    /* git gives the deepest .gitignore the last word: a rule in
     * src/.gitignore beats one in the root .gitignore, negation included,
     * so "!debug.log" in a subdirectory really does re-include a file that
     * the root's "*.log" excluded. Walking the chain from the root and
     * stopping at the first level that ignored the path got that backwards
     * and made a nested negation impossible.
     *
     * The rule this appears to break -- that a negation cannot resurrect
     * something inside an ignored *directory* -- is not enforced here and
     * does not need to be. The tree walk never descends into a directory it
     * has pruned, so no path below one is ever asked about. That is also
     * how git itself gets the same outcome. */
    for (i = chain->count - 1; i >= 0; i--) {
        const IgnoreChainLevel *level = &chain->levels[i];
        const char *sub = rel_path_under(level->base_rel_dir, rel_path);
        int verdict;
        if (!sub) continue;
        verdict = list_verdict(level->list, sub, is_dir);
        if (verdict >= 0) return verdict;
    }
    return 0;
}

/* The verdict of one .gitignore on one path: 1 ignore it, 0 explicitly
 * re-include it, -1 no pattern in this file said anything either way. The
 * chain needs all three to tell "this level is silent" apart from "this
 * level negated a rule from above". */
static int list_verdict(const IgnoreList *list, const char *rel_path, int is_dir) {
    int i;
    int verdict = -1;

    if (!list) return -1;

    for (i = 0; i < list->count; i++) {
        const IgnorePattern *p = &list->items[i];
        int matched;

        if (p->dir_only && !is_dir) continue;

        if (p->anchored) {
            matched = gtree_glob_match(p->pattern, rel_path);
            /* A trailing "double star" suffix should match the directory
             * itself, not just paths underneath it (e.g. "docs" alone
             * should match a pattern written as "docs" plus two stars). */
            if (!matched) {
                size_t plen = strlen(p->pattern);
                if (plen >= 3 && strcmp(p->pattern + plen - 3, "/**") == 0) {
                    char *prefix = xstrdup(p->pattern);
                    if (prefix) {
                        prefix[plen - 3] = '\0';
                        if (strcmp(prefix, rel_path) == 0) matched = 1;
                        free(prefix);
                    }
                }
            }
        } else {
            matched = gtree_glob_match(p->pattern, basename_of(rel_path));
        }

        if (matched) {
            verdict = p->negate ? 0 : 1;
        }
    }

    return verdict;
}

int ignore_list_is_ignored(const IgnoreList *list, const char *rel_path, int is_dir) {
    return list_verdict(list, rel_path, is_dir) == 1;
}
