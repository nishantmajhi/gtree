#!/bin/sh
# run.sh - build gtree and run every test in this folder. POSIX sh, no
# dependencies beyond a C compiler; git is used if present and skipped if not.
#
#   tests/run.sh
#   tests/run.sh --original
#
# This is the counterpart of tests/run.ps1 and runs the same four layers over
# the same fixtures:
#
#   1. gitignore_test   the pattern engine and the nested chain, in process,
#                       against expectations frozen from git
#   2. treeio_test      parsing, filtering and re-rendering real `tree` output
#                       of four dialects
#   3. the differential  the same patterns put to the real git on this machine
#                       via `git check-ignore`, so the frozen expectations
#                       cannot quietly rot
#   4. end to end       the built gtree run over each captured listing,
#                       diffed against tests/expected
#
# --original fetches src/ from the baseline commit with `git show` into a
# temporary directory and compiles the suites against that instead. Nothing is
# transcribed and nothing is copied into the repository, so the assertions
# cannot drift from the code they describe. In that mode the run succeeds when
# every defect named in MUST_REPRODUCE has at least one failing check, which is
# the only thing that establishes those checks test anything at all.

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
tests="$root/tests"
CC=${CC:-cc}
CFLAGS=${CFLAGS:--std=c99 -O1 -Wall -Wextra}

BASELINE=${BASELINE:-main}
BASELINE_SUBJECT='Add support for nested .gitignore files'
MUST_REPRODUCE='D1 D2 D3 D4 D5 D6 D7 D8 D9 D10 D11 E2E'

original=0
keep_build=0
for arg in "$@"; do
    case $arg in
        --original) original=1 ;;
        --keep-build) keep_build=1 ;;
        *) echo "usage: $0 [--original] [--keep-build]" >&2; exit 2 ;;
    esac
done

build=$(mktemp -d 2>/dev/null || mktemp -d -t gtree)
trap 'if [ "$keep_build" -eq 0 ]; then rm -rf "$build"; fi' EXIT INT TERM

results="$build/results.txt"   # one "PASS name" / "FAIL name" per line
: > "$results"

# ------------------------------------------------------------------ #
# Sources under test                                                  #
# ------------------------------------------------------------------ #

src="$root/src"
if [ "$original" -eq 1 ]; then
    subject=$(git -C "$root" log -1 --format=%s "$BASELINE") || exit 1
    if [ "$subject" != "$BASELINE_SUBJECT" ]; then
        echo "baseline '$BASELINE' is '$subject' but these tests were written" >&2
        echo "against '$BASELINE_SUBJECT' -- set BASELINE=<rev>" >&2
        exit 1
    fi
    src="$build/original-src"
    mkdir -p "$src"
    for f in main.c gitignore.c treeio.c gitignore.h treeio.h; do
        git -C "$root" show "$BASELINE:src/$f" > "$src/$f" || exit 1
    done
    echo "# sources: $BASELINE ($subject)"
else
    echo "# sources: the working tree"
fi

compile() {
    out="$build/$1"; shift
    # shellcheck disable=SC2086
    if ! $CC $CFLAGS -I "$src" -I "$tests" -o "$out" "$@"; then
        echo "compiling $out failed" >&2
        exit 1
    fi
    echo "$out"
}

gtree_bin=$(compile gtree      "$src/main.c" "$src/gitignore.c" "$src/treeio.c")
ignore_test=$(compile gitignore_test "$tests/gitignore_test.c" "$src/gitignore.c")
treeio_test=$(compile treeio_test    "$tests/treeio_test.c" "$src/treeio.c" "$src/gitignore.c")
probe_bin=$(compile ignore_probe     "$tests/ignore_probe.c" "$src/gitignore.c")
timing_bin=$(compile timing_probe    "$tests/timing_probe.c" "$src/gitignore.c")

record() { printf '%s\n' "$1" >> "$results"; }

report() { # name, ok(0/1), detail
    if [ "$2" -eq 1 ]; then
        echo "PASS $1"; record "PASS $1"
    else
        echo "FAIL $1 -- $3"; record "FAIL $1"
    fi
}

run_suite() {
    label=$1; shift
    echo "--- $label"
    ( cd "$build" && "$@" ) | while IFS= read -r line; do
        echo "$line"
        case $line in
            PASS\ *|FAIL\ *) printf '%s\n' "$line" >> "$results" ;;
        esac
    done
}

run_suite 'gitignore engine' "$ignore_test"
run_suite 'tree parsing, filtering and rendering' "$treeio_test" "$tests/fixtures"

# ------------------------------------------------------------------ #
# The differential against real git                                   #
# ------------------------------------------------------------------ #

echo "--- differential against real git"
if ! command -v git > /dev/null 2>&1; then
    echo "# SKIPPED: no git on PATH"
else
    repo="$build/gitdiff"
    mkdir -p "$repo"
    ( cd "$repo" && git init -q . && git config core.ignorecase false &&
      git config core.excludesFile "$repo/no-such-global-excludes" )

    # Each case is "<rules, newline-escaped as \n>|<spec> <spec> ...". "f:" is
    # a file, "d:" a directory. The one intended divergence is listed after.
    # Keep this table in step with the one in gitdiff.ps1.
    cases='*.log|f:a.log f:x/a.log f:a.log.txt d:a.log f:log
build/|d:build f:build d:x/build f:builds
/config.json|f:config.json f:src/config.json
src/*.tmp|f:src/a.tmp f:src/sub/a.tmp f:x/src/a.tmp
**/foo|f:foo f:a/foo f:a/b/foo f:foox
a/**/b|f:a/b f:a/x/b f:a/x/y/b f:ab f:x/a/b
a**b|f:ab f:axb f:a/b f:ax/yb
a***b|f:ab f:axb f:a/b
**|f:a f:a/b
[abc].txt|f:a.txt f:d.txt
[a-z].txt|f:q.txt f:Q.txt f:1.txt
[!a].txt|f:a.txt f:b.txt
[^a].txt|f:a.txt f:b.txt
[]].txt|f:].txt f:a.txt
[[:digit:]].c|f:1.c f:a.c f:].c f:d.c
[[:alpha:]].c|f:q.c f:1.c
[[:upper:]].c|f:Q.c f:q.c
[[:digit:]x].c|f:x.c f:7.c f:a.c
?.txt|f:a.txt f:ab.txt
a\\*b|f:a*b f:axb
\\!x|f:!x f:x
\\#y|f:#y f:y
doc|d:doc f:doc f:a/doc d:a/doc
*|f:a f:a/b d:a
*.o\n!keep.o|f:a.o f:keep.o f:d/keep.o
!keep.o\n*.o|f:keep.o
# *.log\nreal|f:a.log f:real
\n\n*.log|f:a.log
/foo/|d:foo d:a/foo
a/b|f:a/b f:x/a/b
**/a/b|f:a/b f:x/a/b f:x/y/a/b
x/**/**/y|f:x/y f:x/a/y f:x/a/b/y
foo/**|f:foo/a f:foo/a/b d:foo
dir|f:dir/keep'

    # "<rules>|<spec>" pairs where gtree and git are meant to disagree. See
    # gitdiff.ps1 for the reasoning; the short version is that the engine is
    # asked about one path at a time and knows nothing of its ancestors, while
    # gtree as a whole prunes the ignored directory instead.
    divergent='dir|f:dir/keep'

    printf '%s\n' "$cases" | while IFS= read -r case_line; do
        rules=${case_line%%|*}
        specs=${case_line#*|}

        # The rules field carries \n for a newline, and a leading BOM case is
        # handled separately below because sh cannot spell one portably.
        printf '%b\n' "$rules" > "$repo/.gitignore"

        : > "$repo/paths.txt"
        for spec in $specs; do
            p=${spec#??}
            case $spec in
                d:*) printf './%s/\n' "$p" >> "$repo/paths.txt" ;;
                *)   printf './%s\n'  "$p" >> "$repo/paths.txt" ;;
            esac
        done

        git_out=$( cd "$repo" && git check-ignore --no-index --stdin -v \
                        --non-matching < paths.txt )
        probe_out=$( "$probe_bin" "$repo/.gitignore" $specs )

        i=0
        for spec in $specs; do
            i=$((i + 1))
            gline=$(printf '%s\n' "$git_out" | sed -n "${i}p")
            pline=$(printf '%s\n' "$probe_out" | sed -n "${i}p")

            # check-ignore prints "::<tab><path>" for no match, and
            # "<file>:<line>:<pattern><tab><path>" for a match -- including a
            # *negated* match, where the path is not ignored after all.
            case $gline in
                ::*) git_says=0 ;;
                *)   pat=$(printf '%s\n' "$gline" | sed 's/^[^:]*:[0-9]*://; s/\t.*//')
                     case $pat in
                         !*) git_says=0 ;;
                         *)  git_says=1 ;;
                     esac ;;
            esac
            gtree_says=${pline%% *}

            key="$rules|$spec"
            if [ "$key" = "$divergent" ]; then
                if [ "$git_says" != "$gtree_says" ]; then
                    report "gitdiff divergence: [$rules] $spec" 1 ''
                else
                    report "gitdiff divergence: [$rules] $spec" 0 \
                           "expected a deliberate disagreement, both said $git_says"
                fi
            elif [ "$git_says" = "$gtree_says" ]; then
                report "gitdiff: [$rules] $spec" 1 ''
            else
                report "gitdiff: [$rules] $spec" 0 "git=$git_says gtree=$gtree_says"
            fi
        done
    done

    # The BOM case, written with printf so the three bytes are exact.
    printf '\357\273\277*.log\n' > "$repo/.gitignore"
    printf './a.log\n./x/a.log\n' > "$repo/paths.txt"
    git_out=$( cd "$repo" && git check-ignore --no-index --stdin -v --non-matching < paths.txt )
    probe_out=$( "$probe_bin" "$repo/.gitignore" f:a.log f:x/a.log )
    i=0
    for spec in f:a.log f:x/a.log; do
        i=$((i + 1))
        gline=$(printf '%s\n' "$git_out" | sed -n "${i}p")
        pline=$(printf '%s\n' "$probe_out" | sed -n "${i}p")
        case $gline in ::*) git_says=0 ;; *) git_says=1 ;; esac
        gtree_says=${pline%% *}
        if [ "$git_says" = "$gtree_says" ]; then
            report "gitdiff: [BOM then *.log] $spec" 1 ''
        else
            report "gitdiff: [BOM then *.log] $spec" 0 "git=$git_says gtree=$gtree_says"
        fi
    done
fi

# ------------------------------------------------------------------ #
# End to end                                                          #
# ------------------------------------------------------------------ #

echo "--- end to end"

# Compare ignoring line endings only: the fixtures carry whatever the checkout
# produced, and the two Windows ones deliberately keep their carriage returns.
normalise() { tr -d '\r' < "$1" | sed -e :a -e '/^$/{$d;N;ba' -e '}'; }

for pair in 'win_tree_f.txt win_tree_f.out tree/F' \
            'win_tree_f_ascii.txt win_tree_f_ascii.out tree/F/A' \
            'unix_utf8.txt unix_utf8.out tree' \
            'unix_ascii.txt unix_ascii.out tree-A'; do
    set -- $pair
    listing="$tests/fixtures/$1"
    expected="$tests/expected/$2"
    label=$3
    actual="$build/actual-$2"

    "$gtree_bin" -i "$listing" -g "$tests/fixtures/project.gitignore" \
                 -r "$tests/fixtures/project" -o "$actual" > /dev/null 2>&1

    if [ -f "$actual" ] && normalise "$actual" | diff -q - "$(
            normalise "$expected" > "$build/want-$2"; echo "$build/want-$2")" \
            > /dev/null 2>&1; then
        report "E2E: end to end, $label" 1 ''
    else
        report "E2E: end to end, $label" 0 'output did not match tests/expected'
        if [ "$original" -eq 0 ] && [ -f "$actual" ]; then
            normalise "$actual" | diff "$build/want-$2" - | sed 's/^/    /'
        fi
    fi
done

# A --root that does not describe the listing has to be reported.
err="$build/rootmismatch.err"
"$gtree_bin" -i "$tests/fixtures/unix_ascii.txt" \
             -g "$tests/fixtures/project.gitignore" -r "$build" \
             > /dev/null 2> "$err"
if grep -q 'not found' "$err" 2>/dev/null; then
    report 'D10: a --root that does not match the listing is reported' 1 ''
else
    report 'D10: a --root that does not match the listing is reported' 0 \
           'nothing was said on stderr'
fi

# ...and one that does match must stay quiet.
err="$build/rootmatch.err"
"$gtree_bin" -i "$tests/fixtures/unix_ascii.txt" \
             -g "$tests/fixtures/project.gitignore" -r "$tests/fixtures/project" \
             > /dev/null 2> "$err"
if grep -q 'not found' "$err" 2>/dev/null; then
    report 'a --root that does match the listing says nothing' 0 "warned needlessly"
else
    report 'a --root that does match the listing says nothing' 1 ''
fi

# A misspelt -g must not look like a .gitignore that ignores nothing.
err="$build/missing.err"
"$gtree_bin" -i "$tests/fixtures/unix_ascii.txt" -g "$build/no-such.gitignore" \
             > /dev/null 2> "$err"
rc=$?
if [ "$rc" -ne 0 ] || grep -q 'no-such.gitignore' "$err" 2>/dev/null; then
    report 'D4: a missing -g file is reported' 1 ''
else
    report 'D4: a missing -g file is reported' 0 \
           'nothing on stderr and the exit code was 0'
fi

# ------------------------------------------------------------------ #
# Non-termination                                                     #
# ------------------------------------------------------------------ #

echo "--- the matcher terminates"
# A hang cannot be reported from inside the process that is hanging, so the
# probe is a separate program and gets ten seconds.
"$timing_bin" '*a*a*a*a*a*a*a*a*a*a*b' \
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaX' > "$build/timing.out" 2>&1 &
probe_pid=$!
waited=0
while kill -0 "$probe_pid" 2>/dev/null && [ "$waited" -lt 10 ]; do
    sleep 1
    waited=$((waited + 1))
done
if kill -0 "$probe_pid" 2>/dev/null; then
    kill -9 "$probe_pid" 2>/dev/null
    wait "$probe_pid" 2>/dev/null
    report 'D5: a star-heavy pattern still terminates' 0 'still running after 10 s'
else
    wait "$probe_pid" 2>/dev/null
    report 'D5: a star-heavy pattern still terminates' 1 ''
fi

# ------------------------------------------------------------------ #
# Verdict                                                             #
# ------------------------------------------------------------------ #

total=$(wc -l < "$results" | tr -d ' ')
failed=$(grep -c '^FAIL ' "$results" || true)
[ -n "$failed" ] || failed=0

echo ""
echo "=================================================="

if [ "$original" -eq 1 ]; then
    echo "against the baseline: $total checks, $failed failed"
    echo ""
    unproven=''
    for tag in $MUST_REPRODUCE; do
        n=$(grep -c "^\(PASS\|FAIL\) $tag: " "$results" || true)
        nf=$(grep -c "^FAIL $tag: " "$results" || true)
        [ -n "$n" ] || n=0
        [ -n "$nf" ] || nf=0
        printf '  %-4s %3s checks, %3s failed against the baseline\n' "$tag" "$n" "$nf"
        if [ "$n" -eq 0 ]; then
            unproven="$unproven $tag(no-check)"
        elif [ "$nf" -eq 0 ]; then
            unproven="$unproven $tag(all-pass)"
        fi
    done
    if [ -n "$unproven" ]; then
        echo ""
        echo "FAILED: these defects are not reproduced by anything, so the fixes for"
        echo "them are unproven:$unproven"
        exit 1
    fi
    echo ""
    echo "OK: every defect in the list is reproduced against the baseline."
    exit 0
fi

echo "$total checks, $failed failed"
if [ "$failed" -ne 0 ]; then
    echo ""
    grep '^FAIL ' "$results" | sed 's/^/  /'
    exit 1
fi
echo "OK"
exit 0
