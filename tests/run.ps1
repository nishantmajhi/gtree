# run.ps1 - build gtree and run every test in this folder.
#
#   powershell -ExecutionPolicy Bypass -File tests\run.ps1
#   powershell -ExecutionPolicy Bypass -File tests\run.ps1 -Original
#
# There are four layers, and each one exists because the one below it
# cannot see the failure:
#
#   1. gitignore_test.exe  the pattern engine and the nested chain, in
#                          process, against expectations frozen from git.
#   2. treeio_test.exe     parsing, filtering and re-rendering real
#                          `tree` output of four dialects.
#   3. gitdiff.ps1         the same patterns put to the real git on this
#                          machine, so the frozen expectations cannot rot.
#   4. the end-to-end pass here: the built gtree.exe run over the captured
#      listings in tests/fixtures, diffed against tests/expected. A
#      passing unit suite says nothing about what the program prints.
#
# -Original compiles layers 1, 2 and 4 against the sources as they were at
# the baseline commit, fetched with `git show` into a temporary directory.
# Nothing is transcribed and nothing is copied into the repository, so the
# assertions cannot drift from the code they describe.
#
# Checks that describe a specific defect are named after it: "D3: ..." for
# the third of the defects listed in review.md, "E2E: ..." for the
# end-to-end pass. In -Original mode the run succeeds when *every* defect
# named in $MustReproduce has at least one failing check, which is the only
# thing that establishes those checks test anything at all. Asking instead
# that every tagged check fail would be the wrong bar: several of them cover
# a dialect or a path the original happened to get right, and a suite that
# had to fail everywhere would have to be thinned until it did.

[CmdletBinding()]
param(
    [switch] $Original,
    [string] $Baseline = 'main',
    [switch] $KeepBuild
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$tests = Join-Path $root 'tests'

# The commit -Original is measured against. Its subject is checked so that
# a moved branch or a rebase fails loudly instead of silently measuring
# the wrong code.
$BaselineSubject = 'Add support for nested .gitignore files'

# ------------------------------------------------------------------ #
# Toolchain                                                           #
# ------------------------------------------------------------------ #

function Find-Gcc {
    $c = Get-Command gcc -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @('D:\TDM-GCC-64\bin\gcc.exe',
                     'C:\TDM-GCC-64\bin\gcc.exe',
                     'C:\msys64\mingw64\bin\gcc.exe')) {
        if (Test-Path $p) { return $p }
    }
    throw "no gcc found: put one on PATH or install MinGW-w64"
}

$gcc = Find-Gcc
$cflags = @('-std=c99', '-O1', '-Wall', '-Wextra')

$build = Join-Path $env:TEMP ("gtree-tests-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $build | Out-Null

# Where the sources under test live: the working tree, or a copy of the
# baseline commit.
$srcDir = Join-Path $root 'src'

if ($Original) {
    $subject = (& git -C $root log -1 --format=%s $Baseline)
    if ($LASTEXITCODE -ne 0) { throw "cannot resolve baseline '$Baseline'" }
    if ($subject -ne $BaselineSubject) {
        throw ("baseline '$Baseline' is '$subject' but these tests were written against " +
               "'$BaselineSubject' -- pass -Baseline <rev> for the right commit")
    }
    $srcDir = Join-Path $build 'original-src'
    New-Item -ItemType Directory -Force $srcDir | Out-Null
    foreach ($f in @('main.c', 'gitignore.c', 'treeio.c', 'gitignore.h', 'treeio.h')) {
        $text = (& git -C $root show "${Baseline}:src/$f")
        if ($LASTEXITCODE -ne 0) { throw "cannot read src/$f from $Baseline" }
        Set-Content -Path (Join-Path $srcDir $f) -Value $text -Encoding utf8
    }
    Write-Output "# sources: $Baseline ($subject)"
} else {
    Write-Output "# sources: the working tree"
}

# Two Windows PowerShell 5.1 traps live in the next few functions, and both
# make a working command look broken. Never merge a native command's stderr
# into the success stream with 2>&1: every line comes back wrapped in an
# ErrorRecord. And even redirecting stderr to a file raises a
# NativeCommandError, which $ErrorActionPreference = 'Stop' turns into a
# thrown exception -- so anything that writes to stderr, a compiler warning
# included, has to be run with the preference relaxed and its exit code
# checked by hand.
function Compile([string] $exe, [string[]] $sources) {
    $out = Join-Path $build $exe
    $errFile = Join-Path $build "$exe.buildlog"
    $gccArgs = $cflags + @('-I', $srcDir, '-I', $tests, '-o', $out) + $sources
    $ErrorActionPreference = 'Continue'
    & $gcc @gccArgs 2> $errFile
    $log = if (Test-Path $errFile) { (Get-Content -Raw $errFile) } else { '' }
    if ($LASTEXITCODE -ne 0) {
        Write-Output $log
        throw "compiling $exe failed"
    }
    # Anything written to the output stream here would be returned by the
    # function along with $out, so warnings are collected and printed by the
    # caller instead.
    if ($log -and $log.Trim()) { $script:buildWarnings += "# warnings building ${exe}:`n$log" }
    return $out
}

$buildWarnings = @()

$engine = Join-Path $srcDir 'gitignore.c'
$treeio = Join-Path $srcDir 'treeio.c'
$mainc  = Join-Path $srcDir 'main.c'

$gtreeExe   = Compile 'gtree.exe'          @($mainc, $engine, $treeio)
$ignoreTest = Compile 'gitignore_test.exe' @((Join-Path $tests 'gitignore_test.c'), $engine)
$treeioTest = Compile 'treeio_test.exe'    @((Join-Path $tests 'treeio_test.c'), $treeio, $engine)
$probeExe   = Compile 'ignore_probe.exe'   @((Join-Path $tests 'ignore_probe.c'), $engine)
$timingExe  = Compile 'timing_probe.exe'   @((Join-Path $tests 'timing_probe.c'), $engine)

foreach ($w in $buildWarnings) { Write-Output $w }

# ------------------------------------------------------------------ #
# Collecting results                                                  #
# ------------------------------------------------------------------ #

$results = New-Object System.Collections.ArrayList

function Record([string] $line) {
    if ($line -match '^(PASS|FAIL) (.+)$') {
        $null = $results.Add([pscustomobject]@{
            Ok   = ($Matches[1] -eq 'PASS')
            Name = $Matches[2]
        })
    }
}

function Run-Suite([string] $exe, [string] $label, [string[]] $suiteArgs) {
    Write-Output "--- $label"
    $errFile = Join-Path $build ([IO.Path]::GetFileName($exe) + '.err')
    $ErrorActionPreference = 'Continue'
    Push-Location $build
    try {
        $out = @(& $exe @suiteArgs 2> $errFile)
    } finally {
        Pop-Location
    }
    # Get-Content -Raw on an empty file returns $null, not "", so the cast
    # has to come before .Trim().
    $errText = if (Test-Path $errFile) { [string](Get-Content -Raw $errFile) } else { '' }
    if ($errText.Trim()) {
        Write-Output "# stderr from ${label}:"
        Write-Output $errText
    }
    foreach ($line in $out) {
        Write-Output $line
        Record ([string] $line)
    }
    if ($LASTEXITCODE -gt 1) {
        $null = $results.Add([pscustomobject]@{ Ok = $false; Name = "$label did not finish (exit $LASTEXITCODE)" })
        Write-Output "FAIL $label did not finish (exit $LASTEXITCODE)"
    }
}

Run-Suite $ignoreTest 'gitignore engine' @()
Run-Suite $treeioTest 'tree parsing, filtering and rendering' @((Join-Path $tests 'fixtures'))

# ------------------------------------------------------------------ #
# The differential against real git                                   #
# ------------------------------------------------------------------ #

Write-Output "--- differential against real git"
$gitdiffOut = @(& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'gitdiff.ps1') -Probe $probeExe)
foreach ($line in $gitdiffOut) { Write-Output $line; Record ([string] $line) }

# ------------------------------------------------------------------ #
# End to end: the program over the captured listings                  #
# ------------------------------------------------------------------ #

Write-Output "--- end to end"

$e2e = @(
    @{ listing = 'win_tree_f.txt';       expected = 'win_tree_f.out';       label = 'tree /F' }
    @{ listing = 'win_tree_f_ascii.txt'; expected = 'win_tree_f_ascii.out'; label = 'tree /F /A' }
    @{ listing = 'unix_utf8.txt';        expected = 'unix_utf8.out';        label = 'tree' }
    @{ listing = 'unix_ascii.txt';       expected = 'unix_ascii.out';       label = 'tree -A' }
)

# Read a file as bytes and turn it into a string via UTF-8, with line endings
# normalised and trailing blank lines dropped. Doing it this way rather than
# with Get-Content is not fussiness: Get-Content decodes using the current
# code page, which turns the UTF-8 box-drawing characters in the expected
# output into mojibake and makes two identical trees compare unequal.
function Read-Normalised([string] $path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    return ($text -replace "`r`n", "`n").TrimEnd("`n")
}

$ErrorActionPreference = 'Continue'

foreach ($c in $e2e) {
    $listing  = Join-Path $tests "fixtures/$($c.listing)"
    $expected = Join-Path $tests "expected/$($c.expected)"
    $name = "E2E: end to end, $($c.label)"

    # -o rather than a pipe, so the bytes gtree writes are the bytes compared.
    $actualFile = Join-Path $build ("actual-" + $c.expected)
    & $gtreeExe -i $listing -g (Join-Path $tests 'fixtures/project.gitignore') `
                -r (Join-Path $tests 'fixtures/project') -o $actualFile | Out-Null

    $actualN = if (Test-Path $actualFile) { Read-Normalised $actualFile } else { '(no output file)' }
    $wantN   = Read-Normalised $expected

    $ok = ($actualN -eq $wantN)
    Write-Output ($(if ($ok) { "PASS $name" } else { "FAIL $name" }))
    if (-not $ok -and -not $Original) {
        Write-Output "  expected:"; foreach ($l in $wantN -split "`n")   { Write-Output "    |$l" }
        Write-Output "  actual:";   foreach ($l in $actualN -split "`n") { Write-Output "    |$l" }
    }
    $null = $results.Add([pscustomobject]@{ Ok = $ok; Name = $name })
}

# A --root that does not describe the listing makes every nested lookup miss,
# so the output is under-filtered with nothing to say so. This is checked
# through the program rather than in gitignore_test.c because it counts on two
# new fields in IgnoreChain, and that suite has to keep compiling against the
# baseline header, which does not have them.
$name = 'D10: a --root that does not match the listing is reported'
$errFile = Join-Path $build 'rootmismatch.err'
$null = & $gtreeExe -i (Join-Path $tests 'fixtures/unix_ascii.txt') `
                    -g (Join-Path $tests 'fixtures/project.gitignore') `
                    -r $build 2> $errFile
$errText = if (Test-Path $errFile) { [string](Get-Content -Raw $errFile) } else { '' }
$ok = ($errText -match 'not found')
Write-Output ($(if ($ok) { "PASS $name" } else { "FAIL $name -- nothing was said on stderr" }))
$null = $results.Add([pscustomobject]@{ Ok = $ok; Name = $name })

# ...and a --root that does match must not produce that warning.
$name = 'a --root that does match the listing says nothing'
$errFile = Join-Path $build 'rootmatch.err'
$null = & $gtreeExe -i (Join-Path $tests 'fixtures/unix_ascii.txt') `
                    -g (Join-Path $tests 'fixtures/project.gitignore') `
                    -r (Join-Path $tests 'fixtures/project') 2> $errFile
$errText = if (Test-Path $errFile) { [string](Get-Content -Raw $errFile) } else { '' }
$ok = ($errText -notmatch 'not found')
Write-Output ($(if ($ok) { "PASS $name" } else { "FAIL $name -- warned about a root that is fine: $errText" }))
$null = $results.Add([pscustomobject]@{ Ok = $ok; Name = $name })

# A misspelled -g must not look like a .gitignore that ignores nothing.
$name = 'D4: a missing -g file is reported'
$errFile = Join-Path $build 'missing.err'
$null = & $gtreeExe -i (Join-Path $tests 'fixtures/unix_ascii.txt') -g (Join-Path $build 'no-such.gitignore') 2> $errFile
$errText = if (Test-Path $errFile) { Get-Content -Raw $errFile } else { '' }
$ok = ($LASTEXITCODE -ne 0) -or ($errText -match 'no-such\.gitignore')
Write-Output ($(if ($ok) { "PASS $name" } else { "FAIL $name -- nothing was said on stderr and the exit code was 0" }))
$null = $results.Add([pscustomobject]@{ Ok = $ok; Name = $name })

# ------------------------------------------------------------------ #
# Non-termination                                                     #
# ------------------------------------------------------------------ #

Write-Output "--- the matcher terminates"
$name = 'D5: a star-heavy pattern still terminates'
$pattern = '*a*a*a*a*a*a*a*a*a*a*b'
$text = ('a' * 50) + 'X'
$proc = Start-Process -FilePath $timingExe -ArgumentList @($pattern, $text) `
            -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $build 'timing.out')
$finished = $proc.WaitForExit(10000)
if (-not $finished) { try { $proc.Kill() } catch {} }
$ok = $finished
Write-Output ($(if ($ok) { "PASS $name -- $((Get-Content -Raw (Join-Path $build 'timing.out')).Trim())" }
                else      { "FAIL $name -- still running after 10 s" }))
$null = $results.Add([pscustomobject]@{ Ok = $ok; Name = $name })

# ------------------------------------------------------------------ #
# Verdict                                                             #
# ------------------------------------------------------------------ #

if (-not $KeepBuild) { Remove-Item -Recurse -Force $build -ErrorAction SilentlyContinue }

$all = @($results)
$total = $all.Count
$failed = @($all | Where-Object { -not $_.Ok }).Count

Write-Output ""
Write-Output "=================================================="

if ($Original) {
    # Every defect the pass claims to have fixed, and the end-to-end pass.
    # A tag with no failing check against the baseline means nothing here
    # reproduces that defect, so the fix for it is unproven.
    $MustReproduce = @('D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8', 'D9', 'D10', 'D11', 'E2E')

    Write-Output "against the baseline: $total checks, $failed failed"
    Write-Output ""

    $unproven = @()
    foreach ($tag in $MustReproduce) {
        $tagged = @($all | Where-Object { $_.Name -like "${tag}: *" })
        $tagFailed = @($tagged | Where-Object { -not $_.Ok })
        Write-Output ("  {0,-4} {1,3} checks, {2,3} failed against the baseline" -f $tag, $tagged.Count, $tagFailed.Count)
        if ($tagged.Count -eq 0) { $unproven += "$tag (no check is named for it)" }
        elseif ($tagFailed.Count -eq 0) { $unproven += "$tag (all its checks pass against the baseline)" }
    }

    $untagged = @($all | Where-Object { -not $_.Ok -and $_.Name -notmatch '^(D\d+|E2E): ' })
    if ($untagged.Count -gt 0) {
        Write-Output ""
        Write-Output "note: $($untagged.Count) check(s) not named for a defect also failed against the"
        Write-Output "baseline. Worth reading. Usually these are the differential's own view of a"
        Write-Output "defect already in the list, since its check names are generated from the"
        Write-Output "patterns rather than written by hand -- but a defect missing from the list"
        Write-Output "above would look exactly the same, so read them."
        foreach ($f in $untagged) { Write-Output "  - $($f.Name)" }
    }

    if ($unproven.Count -gt 0) {
        Write-Output ""
        Write-Output "FAILED: these defects are not reproduced by anything, so the fixes for them"
        Write-Output "are unproven:"
        foreach ($u in $unproven) { Write-Output "  - $u" }
        exit 1
    }
    Write-Output ""
    Write-Output "OK: every defect in the list is reproduced against the baseline."
    exit 0
}

Write-Output "$total checks, $failed failed"
if ($failed -gt 0) {
    Write-Output ""
    foreach ($f in @($all | Where-Object { -not $_.Ok })) { Write-Output "  FAILED $($f.Name)" }
    exit 1
}
Write-Output "OK"
exit 0
