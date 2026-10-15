# gitdiff.ps1 - put gtree's pattern engine and real git side by side.
#
# gtree's whole promise is "show me the tree git would show me", so the
# authority on what a pattern means is the git on this machine, not a
# reading of the gitignore documentation. For each set of rules below this
# writes a real .gitignore into a throwaway repository, asks
# `git check-ignore` about every path in one batch, asks
# tests/ignore_probe.exe the same questions, and reports any disagreement.
#
# One disagreement is intended, and is asserted as a divergence rather than
# quietly tolerated, so that it changing shows up as a failure: the engine
# answers about one path at a time and knows nothing about that path's
# ancestors, so it says a file inside an ignored directory is not itself
# ignored. gtree as a whole gets this right by never descending into a
# pruned directory (see the prune: checks in treeio_test.c), which is also
# how git behaves. The case table therefore avoids paths under an ignored
# directory except for the one case that pins this explicitly.
#
# One agreement is worth knowing the reason for. gtree treats "foo/**" as
# also matching the directory foo, so the directory leaves the listing with
# its contents instead of being drawn as an empty branch. git agrees, but
# only because a directory is spelled with a trailing slash here and
# "foo/" matches "foo/**" textually: asked about "foo" with no slash, git
# says no. Both come to the same thing for anyone reading a listing.
#
# Output is one "PASS <name>" or "FAIL <name>" line per comparison, the
# same format as the C suites, so run.ps1 can aggregate all of it.

param(
    [Parameter(Mandatory = $true)][string] $Probe   # path to ignore_probe.exe
)

$ErrorActionPreference = 'Stop'

$git = (Get-Command git -ErrorAction SilentlyContinue)
if (-not $git) {
    Write-Output "# suite gitdiff -- SKIPPED: no git on PATH"
    exit 0
}
if (-not (Test-Path $Probe)) { throw "ignore_probe not found at $Probe" }
$Probe = (Resolve-Path $Probe).Path

Write-Output "# suite gitdiff"

# Each case is a set of .gitignore lines plus the paths to ask about.
# "f:" is a file, "d:" a directory.
$cases = @(
    @{ rules = @('*.log');        specs = @('f:a.log', 'f:x/a.log', 'f:a.log.txt', 'd:a.log', 'f:log') }
    @{ rules = @('build/');       specs = @('d:build', 'f:build', 'd:x/build', 'f:builds') }
    @{ rules = @('/config.json'); specs = @('f:config.json', 'f:src/config.json') }
    @{ rules = @('src/*.tmp');    specs = @('f:src/a.tmp', 'f:src/sub/a.tmp', 'f:x/src/a.tmp') }
    @{ rules = @('**/foo');       specs = @('f:foo', 'f:a/foo', 'f:a/b/foo', 'f:foox') }
    @{ rules = @('a/**/b');       specs = @('f:a/b', 'f:a/x/b', 'f:a/x/y/b', 'f:ab', 'f:x/a/b') }
    @{ rules = @('a**b');         specs = @('f:ab', 'f:axb', 'f:a/b', 'f:ax/yb') }
    @{ rules = @('a***b');        specs = @('f:ab', 'f:axb', 'f:a/b') }
    @{ rules = @('**');           specs = @('f:a', 'f:a/b') }
    @{ rules = @('[abc].txt');    specs = @('f:a.txt', 'f:d.txt') }
    @{ rules = @('[a-z].txt');    specs = @('f:q.txt', 'f:Q.txt', 'f:1.txt') }
    @{ rules = @('[!a].txt');     specs = @('f:a.txt', 'f:b.txt') }
    @{ rules = @('[^a].txt');     specs = @('f:a.txt', 'f:b.txt') }
    @{ rules = @('[]].txt');      specs = @('f:].txt', 'f:a.txt') }
    @{ rules = @('[[:digit:]].c');   specs = @('f:1.c', 'f:a.c', 'f:].c', 'f:d.c') }
    @{ rules = @('[[:alpha:]].c');   specs = @('f:q.c', 'f:1.c') }
    @{ rules = @('[[:upper:]].c');   specs = @('f:Q.c', 'f:q.c') }
    @{ rules = @('[[:digit:]x].c');  specs = @('f:x.c', 'f:7.c', 'f:a.c') }
    @{ rules = @('?.txt');        specs = @('f:a.txt', 'f:ab.txt') }
    @{ rules = @('a\*b');         specs = @('f:a*b', 'f:axb') }
    @{ rules = @('\!x');          specs = @('f:!x', 'f:x') }
    @{ rules = @('\#y');          specs = @('f:#y', 'f:y') }
    @{ rules = @('doc');          specs = @('d:doc', 'f:doc', 'f:a/doc', 'd:a/doc') }
    @{ rules = @('*');            specs = @('f:a', 'f:a/b', 'd:a') }
    @{ rules = @('*.o', '!keep.o');   specs = @('f:a.o', 'f:keep.o', 'f:d/keep.o') }
    @{ rules = @('!keep.o', '*.o');   specs = @('f:keep.o') }
    @{ rules = @('# *.log', 'real');  specs = @('f:a.log', 'f:real') }
    @{ rules = @('', '', '*.log');    specs = @('f:a.log') }
    @{ rules = @('/foo/');        specs = @('d:foo', 'd:a/foo') }
    @{ rules = @('a/b');          specs = @('f:a/b', 'f:x/a/b') }
    @{ rules = @('**/a/b');       specs = @('f:a/b', 'f:x/a/b', 'f:x/y/a/b') }
    @{ rules = @('x/**/**/y');    specs = @('f:x/y', 'f:x/a/y', 'f:x/a/b/y') }
    # A BOM on the first line: git skips it, so the first rule still works.
    # (Windows PowerShell 5.1 has no `u{} escape, hence the [char] cast.)
    @{ rules = @(([string][char]0xFEFF) + '*.log'); specs = @('f:a.log', 'f:x/a.log') }
    # The pinned divergences.
    @{ rules = @('foo/**');       specs = @('f:foo/a', 'f:foo/a/b', 'd:foo') }
    @{ rules = @('dir');          specs = @('f:dir/keep') }
)

# "<joined rules>|<spec>" => why gtree and git differ on purpose.
$expectedDivergence = @{
    'dir|f:dir/keep' = 'the engine is asked about one path at a time; gtree prunes the ignored directory instead'
}

$repo = Join-Path $env:TEMP ("gtree-gitdiff-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $repo | Out-Null
$run = 0
$failed = 0

function Report([string] $name, [bool] $ok, [string] $detail) {
    $script:run++
    if ($ok) {
        Write-Output "PASS $name"
    } else {
        $script:failed++
        Write-Output "FAIL $name -- $detail"
    }
}

try {
    Push-Location $repo
    & git init -q . 2>$null | Out-Null
    # The repository must decide questions of pattern syntax on its own:
    # no user or system excludes, and no case folding, which on Windows is
    # on by default and would make [a-z] match Q.
    & git config core.ignorecase false
    & git config core.excludesFile ($repo + '/no-such-global-excludes')

    foreach ($case in $cases) {
        $rules = @($case.rules)
        $specs = @($case.specs)
        $label = ($rules -join '\n')

        # Write the rules as bytes so a leading BOM stays a leading BOM.
        $text = ($rules -join "`n") + "`n"
        [System.IO.File]::WriteAllText((Join-Path $repo '.gitignore'), $text,
                                        (New-Object System.Text.UTF8Encoding($false)))

        # Ask git about every path in one call. A directory is spelled with
        # a trailing slash; ./ keeps a path like "!x" from being read as an
        # option, and anchors it inside the repository.
        $paths = foreach ($s in $specs) {
            $p = $s.Substring(2)
            if ($s[0] -eq 'd') { "./$p/" } else { "./$p" }
        }
        # Line endings matter here: `git check-ignore --stdin` without -z
        # splits on newline only, so a CRLF file leaves a carriage return on
        # the end of every path and nothing ever matches. WriteAllLines
        # would write CRLF on Windows, hence the explicit join.
        $pathFile = Join-Path $repo 'paths.txt'
        [System.IO.File]::WriteAllText($pathFile, (($paths -join "`n") + "`n"),
                                        (New-Object System.Text.UTF8Encoding($false)))
        $gitOut = @(& cmd /c "git check-ignore --no-index --stdin -v --non-matching < `"$pathFile`"")

        if ($gitOut.Count -ne $specs.Count) {
            Report "gitdiff: $label" $false "git answered $($gitOut.Count) of $($specs.Count) paths"
            continue
        }

        $probeOut = @(& $Probe (Join-Path $repo '.gitignore') @specs)
        if ($probeOut.Count -ne $specs.Count) {
            Report "gitdiff: $label" $false "ignore_probe answered $($probeOut.Count) of $($specs.Count) paths"
            continue
        }

        for ($i = 0; $i -lt $specs.Count; $i++) {
            $spec = $specs[$i]
            # `check-ignore -v --non-matching` prints "::<tab><path>" when no
            # pattern matched, and "<file>:<line>:<pattern><tab><path>" when
            # one did -- including when the pattern that matched was a
            # negation, in which case the path is *not* ignored. Reading the
            # mere presence of a match as "ignored" gets every ! case wrong.
            $gitSays = 1
            if ($gitOut[$i].StartsWith('::')) {
                $gitSays = 0
            } elseif ($gitOut[$i] -match '^[^\t]*:\d+:(?<pat>[^\t]*)\t') {
                if ($Matches['pat'].StartsWith('!')) { $gitSays = 0 }
            }
            $gtreeSays = [int]($probeOut[$i].Split(' ')[0])
            $key = "$($rules -join '\n')|$spec"

            if ($expectedDivergence.ContainsKey($key)) {
                $ok = ($gitSays -ne $gtreeSays)
                Report "gitdiff divergence: [$label] $spec" $ok `
                    "expected gtree and git to differ here ($($expectedDivergence[$key])) but both said $gitSays"
            } else {
                $ok = ($gitSays -eq $gtreeSays)
                Report "gitdiff: [$label] $spec" $ok "git=$gitSays gtree=$gtreeSays"
            }
        }
    }
} finally {
    Pop-Location
    Remove-Item -Recurse -Force $repo -ErrorAction SilentlyContinue
}

Write-Output "# $run checks, $failed failed"
if ($failed -gt 0) { exit 1 }
exit 0
