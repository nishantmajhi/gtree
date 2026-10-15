# gtree

`tree` shows you everything. In a project with a `node_modules`, that is
tens of thousands of lines you did not want, and the twenty lines you did
want are somewhere inside them.

`gtree` is a filter that takes `tree`'s output and removes whatever your
`.gitignore` would exclude, then draws the tree again with its branches
corrected.

```console
$ tree /F | gtree            # Windows
$ tree | gtree               # Linux / macOS
```

```
C:.
+---README.md
+---src
|   +---index.js
|   \---utils
|       \---helpers.js
\---package.json

2 directories, 4 files
```

It is one C99 program in three files, with no dependencies, no configuration
and no network access. It never writes to your project; it reads text on
standard input and writes text on standard output.

---

## Contents

- [How it works, and what that means for you](#how-it-works-and-what-that-means-for-you)
- [Quick start](#quick-start)
- [Building](#building)
- [Putting gtree on your PATH](#putting-gtree-on-your-path)
- [Options](#options)
- [What of .gitignore is supported](#what-of-gitignore-is-supported)
- [Nested, per-directory .gitignore files](#nested-per-directory-gitignore-files)
- [Where gtree and git disagree](#where-gtree-and-git-disagree)
- [Tests](#tests)
- [Project layout](#project-layout)
- [License](#license)

---

## How it works, and what that means for you

1. It reads `tree` text — from standard input, or from `-i FILE` — and
   parses it back into a tree in memory. It recognises four dialects and
   works out which one it is looking at: Windows `tree /F`, Windows
   `tree /F /A`, Linux/macOS `tree`, and `tree -A`.
2. It loads a root `.gitignore` (`-g FILE`, else `$GTREE_GITIGNORE`, else
   `./.gitignore`), plus `.git/info/exclude` and any per-directory
   `.gitignore` files it finds under `--root`.
3. It walks the parsed tree, working out each entry's path relative to the
   root, and drops anything the rules exclude. A dropped directory takes
   its contents with it. `.git` always goes, whether or not a rule says so.
4. It draws what is left, recomputing every connector, so the entry that is
   now last in a directory gets the corner and not a tee.

**The one thing worth understanding**: gtree's input is *text*, not your
filesystem. It cannot see your project. That is what makes it work on a
listing somebody emailed you, and it is also the source of every limitation
below. Two consequences in particular:

- gtree only knows an entry is a directory if the listing says so — which,
  for plain `tree` and `tree /F`, means "it has something indented under
  it". An **empty** directory is indistinguishable from a file. Use
  `tree -F` on Linux/macOS if you want that removed (it marks directories
  with a trailing `/`).
- Per-directory `.gitignore` files have to be *found*, on a real disk, so
  `--root` has to point at the project the listing came from. If it does
  not, gtree says so on stderr rather than quietly under-filtering.

---

## Quick start

From inside a project that already has a `.gitignore`:

```console
$ tree /F | gtree                       # Windows
$ tree | gtree                          # Linux / macOS
$ gtree                                 # no pipe: gtree runs tree for you
```

Save it instead of reading it:

```console
$ tree /F | gtree -o structure.txt
```

Filter a listing you captured earlier, or one from another machine:

```console
$ gtree -i listing.txt -g path/to/.gitignore -r path/to/project
```

---

## Building

You need a C99 compiler. There is nothing else to install.

**Windows** — any MinGW-w64 gcc (MSYS2, TDM-GCC, w64devkit):

```console
> build.bat
> build.bat test
```

**Linux and macOS**:

```console
$ make
$ make test
$ sudo make install          # to /usr/local/bin; PREFIX=~/.local make install also works
```

macOS needs the command line tools (`xcode-select --install`) for `cc`.
Linux needs `build-essential`, `gcc`, `base-devel` or equivalent, whichever
your distribution calls it.

MSVC is untested. The sources are plain C99 with no compiler extensions, so
`cl /std:c11` would very likely work; nobody has tried it.

---

## Putting gtree on your PATH

gtree is a single executable and can simply live in a folder that is already
on your PATH. If you would rather add its own:

**Windows**, for the current user, from PowerShell:

```powershell
$dir = "C:\tools\gtree"          # wherever gtree.exe is
[Environment]::SetEnvironmentVariable(
    "Path",
    [Environment]::GetEnvironmentVariable("Path", "User") + ";$dir",
    "User")
```

Open a new terminal afterwards; existing ones keep the old PATH.

**Linux and macOS** — `make install` puts it in `/usr/local/bin`, which is
normally on PATH already. For a user-local install:

```console
$ PREFIX="$HOME/.local" make install
$ echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc   # or ~/.zshrc
```

Check it worked with `gtree --version`.

---

## Options

```
  -i, --input FILE       read tree text from FILE (default: stdin)
  -g, --gitignore FILE   the root .gitignore
                         (default: $GTREE_GITIGNORE, else ./.gitignore)
  -r, --root DIR         the project directory the listing was made from,
                         used to find per-folder .gitignore files and
                         .git/info/exclude
                         (default: the directory part of -g, else ".")
  -o, --output FILE      write to FILE (default: stdout)
  -v, --verbose          report what was loaded, on stderr
  -h, --help             show this help
      --version          show the version
```

With no `-i` and nothing piped in, gtree runs `tree /F` or `tree` for you
rather than sitting waiting for you to type a directory listing.

`-g` naming a file that cannot be read is an error, and gtree exits 1
rather than printing an unfiltered tree that looks filtered. A `.gitignore`
that is merely *absent* when you never asked for a particular one is fine
and means "nothing is ignored".

---

## What of .gitignore is supported

| Feature | Example |
| --- | --- |
| Comments and blank lines | `# comment` |
| Match a basename at any depth | `*.log` |
| Directories only | `build/` |
| Anchored to the root | `/config.json` |
| Anchored by an inner slash | `src/*.tmp` |
| Wildcards | `*`, `?` |
| Character classes and ranges | `[abc]`, `[a-z]`, `[!a]`, `[^a]` |
| POSIX classes | `[[:digit:]]`, `[[:alpha:]]`, `[[:upper:]]` … |
| Globstar | `**/foo`, `foo/**`, `a/**/b` |
| Negation | `!important.log` |
| Escapes | `\!literal`, `\#literal`, `\*literal` |
| A UTF-8 BOM on the first line | skipped, as git does |
| CRLF line endings | fine |
| `.git/info/exclude` | read, at lower precedence than any `.gitignore` |
| `.git` | always excluded |

Within one file the last matching pattern wins, as in git. Across files, the
deepest `.gitignore` wins — see below.

Not read: `.gitattributes`, your global excludes file (`core.excludesFile`,
usually `~/.config/git/ignore`) and any system-wide one. Both would mean
parsing git configuration, which is a larger job than the rest of this
program.

---

## Nested, per-directory .gitignore files

Real projects put a `.gitignore` in subdirectories. gtree applies them, but
it has to find them on disk, because the listing is only text:

```console
$ tree | gtree -g project/.gitignore
$ tree | gtree -g project/.gitignore -r project    # the explicit form
```

`--root` is where gtree looks. For every directory it walks into it checks
`<root>/<that path>/.gitignore`, and layers what it finds on top of the
rules from above. A subdirectory without one simply inherits.

Precedence is git's: the deepest file that says anything about a path
decides, **negation included**. So this re-includes the log file:

```
.gitignore              *.log
frontend/.gitignore     !debug.log
frontend/debug.log      kept
```

A negation cannot bring back something inside an **ignored directory**,
because gtree never descends into a directory it has already dropped —
which is also how git ends up with the same answer.

If `--root` does not describe the listing (it came from another machine, or
from before a rename), those lookups find nothing and the output is missing
rules. gtree counts the directories it could not find and says so on
stderr, once.

---

## Where gtree and git disagree

The honest list. Everything here is pinned by a test, so it cannot change
without somebody noticing.

- **Empty directories.** Plain `tree` and `tree /F` do not mark
  directories, so gtree infers "directory" from "has children in the
  listing". An empty directory therefore looks like a file, and a
  directory-only rule (`build/`) will not match it. A name-based rule
  (`build`) still will. `tree -F` removes the ambiguity.
- **Case.** gtree matches case-sensitively, like git with
  `core.ignorecase=false`. On Windows git defaults to `true`, where `*.LOG`
  would also exclude `a.log`; gtree will not. Matching git here means
  reading git's configuration, which gtree deliberately does not do.
- **A trailing globstar and the directory it names.** Given `foo/**`, gtree
  treats the directory `foo` as matched too, so it leaves the listing along
  with its contents instead of being drawn as an empty branch.
  `git check-ignore` reports `foo` as not ignored and everything under it as
  ignored, which comes to the same thing for anyone reading a listing.
- **Re-rendering.** gtree redraws the tree in a canonical style for the
  dialect it detected rather than reproducing the input byte for byte. In
  particular, Windows `tree /F` gives files no connector at all and uses a
  non-ASCII byte for its vertical bars; gtree's output uses `+---` and
  `|` throughout, like `tree /F /A`.

---

## Tests

```console
$ make test                                                 # Linux / macOS
> build.bat test                                            # Windows
> powershell -ExecutionPolicy Bypass -File tests\run.ps1    # Windows, no rebuild
```

304 checks in four layers, no test framework and nothing to install:

1. **The pattern engine and the nested chain**, in process
   (`tests/gitignore_test.c`).
2. **Parsing, filtering and re-rendering** (`tests/treeio_test.c`), over
   four listings in `tests/fixtures`. The two Windows ones are byte-exact
   captures of `tree /F` and `tree /F /A`, which matters more than it
   sounds: an earlier hand-written sample was wrong about the format, and
   that is why several defects survived so long.
3. **A differential against the real git on your machine**, which puts each
   pattern to `git check-ignore` and compares its answer with gtree's. The
   expectations in layer 1 are frozen copies of git's answers; this is what
   stops them rotting. Skipped if git is not on PATH.
4. **End to end**: the built binary over each listing, diffed against
   `tests/expected`.

Both runners also take `--original` (`make test-original`, or
`tests/run.ps1 -Original`), which fetches `src/` from before this project's
cleanup pass with `git show` and compiles the suite against that instead.
It then asserts that every defect the pass claims to have fixed has at least
one check that fails — which is the only thing that establishes those checks
test anything at all. `review.md` lists the defects by the numbers the check
names use.

---

## Project layout

```
gtree/
├── src/
│   ├── main.c            CLI: arguments, input, output
│   ├── gitignore.c/h     the .gitignore engine and the nested chain
│   └── treeio.c/h        parsing, filtering and rendering tree text
├── tests/
│   ├── run.ps1           the whole suite, Windows
│   ├── run.sh            the whole suite, POSIX
│   ├── gitignore_test.c  layer 1
│   ├── treeio_test.c     layer 2
│   ├── gitdiff.ps1       layer 3
│   ├── ignore_probe.c    the engine as a command, for layer 3
│   ├── timing_probe.c    a matcher call that can be timed out
│   ├── harness.h         assertions
│   ├── fixtures/         the four listings and the rules applied to them
│   └── expected/         what each one should filter down to
├── Makefile              build, test and install for Linux and macOS
├── build.bat             build and test for Windows
├── review.md             what was wrong with this project, and what is left
└── LICENSE
```

## License

Free for personal, non-commercial use. You may use, study and modify this
software for yourself, but you may not use it commercially, and you may not
share or publish it, in its original form or modified. See [LICENSE](LICENSE)
for the full terms.
