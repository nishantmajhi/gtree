# gtree — review and remediation record

First reviewed 2026-09-04. Remediated 2026-09-27 on branch
`cleanup/2026-09-27`. Not pushed.

---

## Correction to the first review, at the top because it matters

**The first review called this "the best-engineered C project in the archive
and one of the few here I would call finished", found "nothing critical", and
said its notes were "improvements to something that already works, not
repairs." The first two sentences of that are defensible. The third is not.**

`tree /F | gtree` — the first example in the README, the first example in the
program's own `--help`, and the way a Windows user would actually run this —
produced nonsense. Not a subtly wrong tree: a tree with almost no files in it,
with an entry whose name is a piece of line art, with directories in it that
the `.gitignore` excluded, and with a summary counting things that are not
there.

Both of these are real output, from `tests/fixtures/win_tree_f.txt` (a capture
of `tree /F` over a seven-file project) and `tests/fixtures/project.gitignore`
(`build/`, `*.log`, `node_modules/`):

```
before                          after
------------------------        ------------------------
C:.                             C:.
+---build                       +---README.md
+---empty                       +---empty
+---node_modules                \---src
+---src                             +---index.js
+---�   index.js                    \---utils
\---�                                   \---helpers.js
    \---utils
        \---helpers.js          2 directories, 4 files

2 directories, 6 files
```

`build` and `node_modules` are both excluded by that `.gitignore` and are in
the output anyway. `README.md` and `debug.log` are gone from it entirely. Two
entries are punctuation — that is the byte `0xA6`, which is what `tree /F`
writes for a vertical bar.

**How a review of this quality missed it**: it read the code, and it read the
project's own sample listing in `test/`, and the two agreed with each other.
The sample was hand-built and was not what `tree /F` emits. Nobody had run the
program against real `tree` output — which takes about fifteen seconds, and is
the only step that could have found this. The reviewer's own closing note said
"verify with `cd test && ../gtree -i sample_windows.txt`", which is exactly the
check that could not fail.

The lesson generalises past this project and is now in `CONTINUE-HERE.md`: **a
fixture that came from a person rather than from the tool it imitates is a
statement of what somebody believed the format to be.** Two of the four
listings in `tests/fixtures` are byte-exact captures for that reason, and
`tests/fixtures/README.md` says which.

Two smaller corrections:

- The review's point 4 says `dirname_of_path` returning `"\\"` for `\foo`
  produces `\/.gitignore`, and that "Windows tolerates mixed separators so it
  works... worth normalising for clarity". It is not only a clarity matter. A
  root of `/` or `\` produces `//.gitignore`, which Windows reads as the start
  of a **UNC path** and resolves by looking for a machine on the network named
  `.gitignore`. Fixed, though see "What is not proven" below.
- The review's checklist has the test suite and the committed binary as equal
  P2s. The binary was a thirty-second `git rm --cached`. The missing suite was
  the reason nine defects were live.

---

## What was fixed

Numbered as the tests name them: `tests/run.ps1 -Original` (or
`make test-original`) asserts that each of these has at least one check that
fails against the code as it was before this pass.

| | Defect | Severity |
| --- | --- | --- |
| D1 | A spacer line in `tree /F` output became an entry named `|`, and the rest of the tree was re-parented under it | P1 |
| D2 | The `tree /F` continuation byte was unrecognised, so every file line was silently discarded | P1 |
| D3 | A UTF-8 BOM on `.gitignore` silently disabled its first pattern | P2 |
| D4 | A `-g` file that could not be read printed an unfiltered tree and exited 0 | P2 |
| D5 | The glob matcher did not always terminate | P2 |
| D6 | A nested `.gitignore` could add rules but never negate one | P2 |
| D7 | POSIX bracket expressions were parsed into a different pattern | P3 |
| D8 | A non-English `tree` header became the tree's root | P3 |
| D9 | `keep` stayed set on every node inside a pruned subtree | P3 |
| D10 | A `--root` that did not describe the listing filtered less, silently | P3 |
| D11 | `.git/info/exclude` was not read | P3 |

**D1 and D2 are the pass.** Both are in how `tree /F` really writes its output,
and both were invisible because the project's only fixture was wrong about it.

`tree /F` gives a **file** no connector glyph at all — only directories get
`+---` or `\---`, and a file is indented one group deeper and named directly.
The parser coped with that, via a branch that warned "unrecognized connector,
best-effort parse" for most of the lines in its main input.

The **vertical bar is not ASCII `|`**. A redirected `tree /F` writes the single
byte `0xA6`; a console in codepage 437 or 850 uses `0xB3`; ASCII `|` appears
only with `/A`. The parser knew `"|   "` and nothing else, so in the default
dialect every file line had indentation it could not read, counted as depth
zero and was dropped. The knock-on is the part that makes it a P1 rather than a
cosmetic loss: a directory whose only contents were files ended up with no
children, was therefore not recognised as a directory, and stopped matching
directory-only rules — so `build/` in a `.gitignore` quietly did nothing.

And between a directory's files and its subdirectories `tree /F` writes a line
of indentation and nothing else. Trimming its trailing spaces leaves a bare
vertical bar, which became an entry named `|`: a phantom directory sitting at
exactly the depth the next line expected its parent to be, so everything after
it was re-parented underneath. Every relative path gained a segment. **That is
why the nested-`.gitignore` feature — the headline of the last commit before
this branch, and a whole README section — had never worked on Windows input at
all.** It was looking for `|/src/.gitignore` on disk.

D6 is worth reading if you touch the chain. git gives the **deepest**
`.gitignore` the last word, negation included, so `!debug.log` in
`frontend/.gitignore` really does re-include a file the root's `*.log`
excluded. The chain was walked root-first and returned on the first level that
ignored the path, which makes a nested negation impossible. This had been
documented in the README as a deliberate simplification, "slightly stricter
than git's handling of individual files". Deliberate or not, it made the tool
answer a different question from the one it advertises, and the fix was
cheaper than the paragraph excusing it.

D5 is the one to know about if you write a matcher. `match_here` is plain
recursive backtracking, so `*a*a*a*a*a*a*a*a*a*a*b` against fifty `a`s and one
wrong character never comes back. The function is pure, so memoising which
(pattern position, text position) pairs have already failed bounds it at
O(pattern × text) and cannot change an answer.

---

## What was deliberately not done

**Case-insensitive matching.** gtree matches case-sensitively, like git with
`core.ignorecase=false`. On Windows git defaults to `true`, so there `*.LOG`
also excludes `a.log` and gtree will not. Matching git properly means reading
`core.ignorecase` out of `.git/config`, and guessing from the platform would be
wrong for any repository that sets it explicitly. Parsing git configuration is
a bigger job than the whole of the rest of this program. Documented in the
README instead, and pinned by `glob: range is case sensitive` so it cannot
change silently.

**`.gitattributes`, `core.excludesFile` and the system ignore file.** Same
reason: all three need git's configuration parsed. `.git/info/exclude` was
added (D11) precisely because it needs none — it is at a fixed path under
`--root` and in the `.gitignore` format exactly.

**The empty-directory ambiguity.** Inherent to the input format, as the first
review correctly said. `tree` does not mark directories, so gtree infers
"directory" from "has children in the listing", and an empty directory looks
like a file. It is counted as a file in the summary, and a directory-only rule
will not match it. `tree -F` removes the ambiguity and gtree understands it.

**Rewriting history to remove the committed Linux binary.** It is untracked
now, and out of the working tree, but the blob is still in the history — and
this repository has a public remote (`github.com/nishantmajhi/gtree`), so it is
published. It carries nothing sensitive: a 30 KB ELF x86-64 executable built
from the sources next to it, stale from the first commit after it. Removing it
means rewriting history, which affects every clone, so it is the author's call
and not this pass's. Nothing depends on it going.

**A platform separator macro.** The review's point 5 asked for one. Paths built
here are only ever passed to `fopen` and `stat`, both of which take `/` on
Windows, and they are never displayed — so a macro would add a conditional
without removing a risk. The actual risk in that area was the doubled
separator, which is fixed on its own terms.

---

## What is not proven

One change in this pass is reasoned rather than demonstrated, and it should be
read as such: **the doubled-separator fix**. Reproducing the UNC case needs a
`--root` of exactly `/` or `\`, which means writing to the root of a drive, and
no test here is going to do that. What the suite pins is the ordinary case — a
trailing separator on a normal root still finds a nested `.gitignore`. The
reasoning is in the commit and in the comment on `build_fs_path`.

Everything else in the table above fails against the baseline and passes
against this branch, checked both ways with both runners.

---

## Behaviour changes beyond repair

Two, both flagged rather than buried:

1. **A `-g` or `$GTREE_GITIGNORE` file that cannot be read is now an error**
   (exit 1) instead of being treated as an empty `.gitignore`. An invocation
   that used to print a whole tree and exit 0 now prints two lines on stderr
   and exits 1. Absence when nobody named a file is still fine.
2. **A new warning on stderr** when directories in the listing are not found
   under `--root`. It is one message per run, not per directory, and stdout is
   untouched, so a redirected listing is unaffected.

Neither changes the output of a successful run. No UI or layout was touched;
there is none.

---

## The tests

304 checks, no framework, nothing to install. `make test` on Linux and macOS,
`build.bat test` on Windows, or either runner directly. Four layers, and the
reason for each is that the one below it cannot see the failure:

1. `tests/gitignore_test.c` — the pattern engine and the nested chain, in
   process.
2. `tests/treeio_test.c` — parsing, filtering and re-rendering, over the four
   listings in `tests/fixtures`.
3. `tests/gitdiff.ps1` and the same table inside `tests/run.sh` — every pattern
   put to the **real git on this machine** via `git check-ignore`, compared
   with gtree's answer. The expectations in layer 1 are frozen copies of git's
   answers; this is what stops them rotting. Skipped if git is absent.
4. End to end in the runners — the built binary over each listing, diffed
   against `tests/expected`.

Three things in here are worth stealing for another project:

**Ask the reference implementation instead of asserting the documentation.**
The differential is what established that the engine was right about far more
than the review credited it with — run against the baseline it reports 85 of
its 92 comparisons already agreeing with git, and the seven that did not are
D3 and D7 — and it is what found D7 in the first place. Writing out what the
gitignore documentation says would have got both wrong in the same direction
the code did.

**`git check-ignore -v` prints a line for a negated match too.** Reading the
presence of a match as "ignored" gets every `!` case backwards. And
`--stdin` without `-z` splits on newline only, so a CRLF paths file leaves a
carriage return on the end of every path and *nothing matches* — which looks
exactly like a broken engine, and cost a debugging round.

**A hang needs a separate process.** A check cannot report non-termination from
inside the process that is not terminating. `tests/timing_probe.c` is a
three-line program the runners give ten seconds.

Two Windows PowerShell 5.1 traps are recorded in the header of
`tests/run.ps1`, both of which make a working command look broken: `2>&1` on a
native command wraps every stderr line in an ErrorRecord, and even redirecting
stderr to a *file* raises a NativeCommandError, which
`$ErrorActionPreference = 'Stop'` turns into a thrown exception — so a compiler
warning aborts the script. Also: `Write-Output` inside a function becomes part
of its return value, so a function that logs and returns a path returns both.

---

## Notes for whoever is next

**The `-Original` mode constrains what the C suites may reference.** They are
compiled against the baseline commit's headers, so a test that uses a struct
field added by this pass will not build in that mode. D10's coverage lives in
the runners, as a check on the program's stderr, for exactly that reason.
Remember it before adding a field a test wants.

**`tests/fixtures`' two Unix listings are hand-written**, because no machine
here has GNU `tree`. They follow the documented format and the suite is green
against them, but they are not captures and the fixtures README says so. If you
are on Linux or macOS, replacing them with real output is a two-command job and
would close the last gap in that layer.

**The version number is still hand-maintained.** `GTREE_VERSION` in `main.c`
says 1.1.0 and nothing ties it to a tag. Left alone: it is a release-process
question, not a code one, and this pass created no release.

**There is no CI.** Both runners exit non-zero on failure and need nothing but
a compiler and, optionally, git, so either would drop into a workflow file
unchanged.

The delicate part remains `gitignore.c`'s matcher, as the first review said.
It is now covered from three directions, so before simplifying any of it,
change it and watch which layer complains.

## Licence, 2026-10-02

The author chose one licence for all of their projects: free for personal,
non-commercial use, modifiable for the user's own use, and not to be
distributed, original or modified. No standard licence says that, so `LICENSE`
is a short custom text, the same in every project apart from the year and what
it excludes. The README's licence section says the same. Anything above that
calls the licence an open question, or calls the project MIT, is superseded.

This repository's `origin/main` already publishes an MIT `LICENSE`. Copies
taken under it keep MIT; the new licence governs the code from this branch on.

## CI, 2026-10-02

`.github/workflows/ci.yml` runs `make test` on `ubuntu-latest` and `build.bat
test` on `windows-latest`. Nothing has been pushed, so it has not run on
GitHub yet. Every job was run before committing, from a fresh clone of this
branch, the way a runner would run it: Linux jobs in a throwaway Ubuntu 24.04
(the image `ubuntu-latest` uses), Windows jobs here, each with only the tools
the job sets up. 304 on each.

**Fixed on the way:** gtree no longer compiled on Linux. `directory_exists()`,
added by `53c7394` in the 2026-09-27 pass, used `S_IFMT` and `S_IFDIR`, which
glibc hides under the `-std=c99` the Makefile and scripts use; that pass ran
only the Windows suite. It now uses POSIX `S_ISDIR`, checked under `-std=c99`
with glibc, TDM-GCC 10 and MinGW-w64 gcc 16. The published `main` never had
the function.

`tests/run.sh` carries on after a failed compile with an empty binary path, so
the failure showed as some hundred `Permission denied` and `FAIL gitdiff`
lines rather than one compile error. Worth making it stop at the first failed
compile.
