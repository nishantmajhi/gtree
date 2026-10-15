# Fixtures

Four listings of the same eleven-entry tree, one per `tree` dialect that
gtree claims to read, plus the `.gitignore` files the end-to-end pass
applies to them.

```
README.md          src/app.log
debug.log          src/index.js
build/out.bin      src/utils/helpers.js
empty/             node_modules/pkg/p.js
```

| File | Where it came from |
| --- | --- |
| `win_tree_f.txt` | byte-exact `tree /F > file` on Windows 11 |
| `win_tree_f_ascii.txt` | byte-exact `tree /F /A > file` on Windows 11 |
| `unix_utf8.txt` | written by hand from GNU `tree`'s documented output |
| `unix_ascii.txt` | written by hand from GNU `tree -A`'s documented output |

The distinction matters, so it is recorded rather than glossed. The two
Windows files are captures and are the reason this pass exists: the
hand-written sample listing the project shipped before gave files a `+---`
connector, which real `tree /F` does not, and used an ASCII `|` for the
vertical bar, which real `tree /F` does not either — it writes the single
byte `0xA6`. Every conclusion anyone had drawn about gtree working on
Windows had been drawn from that inaccurate sample.

No machine here has GNU `tree`, so the two Unix listings could not be
captured the same way. They are hand-written, and anyone who has a Linux or
macOS box should feel free to replace them with the real thing:

```sh
cd some/project && tree      > unix_utf8.txt
cd some/project && tree -A   > unix_ascii.txt
```

The tests read all four from here rather than embedding copies, so a
replacement takes effect everywhere at once. `tests/expected/` holds the
filtered output each one should produce.

## The rules applied to them

`project.gitignore` is the root `.gitignore` for the end-to-end pass:
`build/`, `*.log` and `node_modules/`.

`project/` is the filesystem root passed as `--root`. It exists so that the
nested, per-directory lookup is exercised end to end: `project/src/.gitignore`
re-includes `*.log`, which is what makes `src/app.log` survive a root rule
that excludes it.

It mirrors only the directories the filtered walk actually enters, which is
`src` and `src/utils` — `build` and `node_modules` are excluded by the root
rules and never descended into, and `empty` has no children so gtree reads it
as a file. `src/utils` holds a `.gitkeep` only because git does not track
empty directories and the directory has to exist: gtree now warns when a
directory in the listing is not there under `--root`, since that means its
`.gitignore` could not have been applied, and a fixture that provoked its own
warning would be a confusing thing to read.
