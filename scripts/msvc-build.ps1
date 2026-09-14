<#
.SYNOPSIS
    Builds pg_textsearch.dll with MSVC against a PostgreSQL 17 or 18 x64 tree.

.DESCRIPTION
    PGXS cannot drive MSVC, so this script is the Windows build recipe. It
    reads OBJS and DATA from the Makefile so there is no second source list
    to keep in sync, compiles every object with /W3 /WX, links against
    postgres.lib and runs two gates:

      layout gate  src/layout_check.c checks every packed or aligned on-disk
                   struct against the reference GCC x86-64 layout. If MSVC
                   produces a different layout, compilation stops.
      export gate  every C symbol used through MODULE_PATHNAME in the SQL
                   files must be exported by the DLL, so a missing PGDLLEXPORT
                   fails here instead of when PostgreSQL loads the library.

    The script writes only into WorkDir: never into PgRoot, nothing installed.

.PARAMETER PgRoot
    Root of a PostgreSQL 17 or 18 x64 tree, either an installation or an
    extracted EDB binaries zip. Must contain include\server and
    lib\postgres.lib.

.PARAMETER WorkDir
    Scratch and output directory. Defaults to <repo>\target\msvc.

.PARAMETER ExpectedMajor
    Refuse to build unless the PgRoot headers report this major version.
    Omitted, any of 17 or 18 is accepted.

.EXAMPLE
    .\scripts\msvc-build.ps1 -PgRoot 'C:\Program Files\PostgreSQL\18'

.EXAMPLE
    .\scripts\msvc-build.ps1 -PgRoot D:\pg17\pgsql -ExpectedMajor 17
#>
[CmdletBinding()]
param(
    [string]$PgRoot,
    [string]$WorkDir,
    [string]$ExpectedMajor
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$global:LASTEXITCODE = 0

$SupportedMajors = @('17', '18')

# MSVC version of the flag block in the Makefile. Use /MD: postgres.exe uses
# the dynamic CRT, and /MT would give the extension its own second CRT heap.
# /WX works like -Werror on Unix.
$CompileFlags = @(
    '/nologo'
    '/c'
    '/MD'
    '/O2'
    '/std:c11'
    '/utf-8'
    '/W3'
    '/WX'
    # Conversion warnings that already occur on main; gcc/clang -Wall -Wextra
    # does not report them. Nothing else is suppressed, so any new warning
    # still fails the build.
    '/wd4244'
    '/wd4267'
    '/wd4305'
)

$CompileDefines = @(
    '/DWIN32'
    '/D_WINDOWS'
    '/D_CRT_SECURE_NO_WARNINGS'
    '/D_CRT_NONSTDC_NO_DEPRECATE'
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Write-Step {
    param([string]$Message)
    Write-Host ''
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Detail {
    param([string]$Message)
    Write-Host "    $Message"
}

function Get-FullPath {
    param([string]$Path, [string]$BaseDir)
    return [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($BaseDir, $Path))
}

function New-CleanDirectory {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Write-TextFile {
    param([string]$Path, [string[]]$Lines)
    $content = ($Lines -join "`r`n") + "`r`n"
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $content, $utf8NoBom)
}

function Find-VcVars {
    $candidates = New-Object System.Collections.Generic.List[string]
    $pf86 = ${env:ProgramFiles(x86)}
    if ($pf86) {
        $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $found = & $vswhere -products '*' -latest `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath
            $global:LASTEXITCODE = 0
            foreach ($install in $found) {
                if ($install) {
                    $candidates.Add((Join-Path $install.Trim() 'VC\Auxiliary\Build\vcvars64.bat'))
                }
            }
        }
        $candidates.Add((Join-Path $pf86 'Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'))
    }
    $pf = ${env:ProgramFiles}
    if ($pf) {
        foreach ($edition in @('Enterprise', 'Professional', 'Community', 'BuildTools')) {
            $candidates.Add((Join-Path $pf "Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat"))
        }
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'vcvars64.bat not found. Install Visual Studio 2022 Build Tools with the "Desktop development with C++" workload.'
}

function Import-VcVarsEnvironment {
    param([string]$VcVarsPath)
    # vcvars64.bat changes the environment of its own cmd.exe only, so run
    # it in a child shell and copy the result into this process. The call
    # goes through a temporary .cmd file because pwsh and cmd.exe escape
    # embedded quotes differently.
    $marker = 'VCVARS_ENVIRONMENT_FOLLOWS'
    $shim = Join-Path ([System.IO.Path]::GetTempPath()) ('vcvars-' + [System.Guid]::NewGuid().ToString('N') + '.cmd')
    Write-TextFile -Path $shim -Lines @(
        '@echo off'
        ('call "' + $VcVarsPath + '" >nul 2>&1')
        'if errorlevel 1 exit /b 1'
        "echo $marker"
        'set'
    )
    try {
        $lines = & cmd.exe /c $shim
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to run $VcVarsPath (exit code $LASTEXITCODE)"
        }
    }
    finally {
        Remove-Item -LiteralPath $shim -Force -ErrorAction SilentlyContinue
    }
    $started = $false
    foreach ($line in $lines) {
        if (-not $started) {
            if ($line -eq $marker) { $started = $true }
            continue
        }
        $split = $line.IndexOf('=')
        if ($split -lt 1) { continue }
        [System.Environment]::SetEnvironmentVariable(
            $line.Substring(0, $split), $line.Substring($split + 1))
    }
    if (-not $started) {
        throw "Could not capture the environment from $VcVarsPath"
    }
}

function Get-MakefileVariable {
    param([string]$MakefilePath, [string]$Name)
    $lines = Get-Content -LiteralPath $MakefilePath
    # Exactly one plain assignment may define the variable. A second one
    # (+=, :=, ?=, a conditional block) means a Makefile shape this small
    # parser cannot evaluate; silently ignoring it would drop objects or SQL
    # files from the Windows build, so it fails instead.
    $assigns = @($lines | Select-String -Pattern ('^\s*' + $Name + '\s*[:+?]?='))
    if ($assigns.Count -eq 0) { throw "Variable $Name not found in $MakefilePath" }
    if ($assigns.Count -gt 1) {
        $at = ($assigns | ForEach-Object LineNumber) -join ', '
        throw "$Name has $($assigns.Count) assignments in $MakefilePath (line $at);" +
            ' this parser supports exactly one'
    }
    $buffer = New-Object System.Collections.Generic.List[string]
    foreach ($line in $lines[$($assigns[0].LineNumber - 1)..($lines.Count - 1)]) {
        $continues = ($line.TrimEnd() -match '\\$')
        $buffer.Add(($line -replace '\\\s*$', ''))
        if (-not $continues) { break }
    }
    $joined = ($buffer -join ' ') -replace ('^\s*' + $Name + '\s*=\s*'), ''
    $values = @($joined -split '\s+' | Where-Object { $_ -ne '' })
    if ($values.Count -eq 0) { throw "Variable $Name is empty in $MakefilePath" }
    return $values
}

function Invoke-Cl {
    param([string]$Tag, [string]$RspDir, [string]$ObjDir, [string[]]$Flags, [string[]]$Sources)
    New-Item -ItemType Directory -Path $ObjDir -Force | Out-Null
    $lines = @($Flags)
    # The doubled backslash survives cl's argument unescaping as one trailing
    # separator, which is what makes /Fo name a directory instead of a file.
    $lines += ('/Fo"' + $ObjDir + '\\"')
    foreach ($source in $Sources) { $lines += ('"' + $source + '"') }
    $rspName = "cl-$Tag.rsp"
    Write-TextFile -Path (Join-Path $RspDir $rspName) -Lines $lines
    # Run from the response directory so the @ argument needs no quoting.
    Push-Location -LiteralPath $RspDir
    try {
        & cl.exe "@$rspName"
        if ($LASTEXITCODE -ne 0) { throw "cl.exe failed for $Tag (exit code $LASTEXITCODE)" }
    }
    finally {
        Pop-Location
    }
}

function Get-DllExport {
    param([string]$DllPath)
    # link /dump prints what dumpbin prints. /dump must come first, or
    # link.exe treats the DLL as an input object.
    $output = & link.exe /dump /nologo /exports $DllPath
    if ($LASTEXITCODE -ne 0) { throw "link /dump /exports failed for $DllPath" }
    $names = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
    foreach ($line in $output) {
        $match = [regex]::Match($line, '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)')
        if ($match.Success) { $names.Add($match.Groups[1].Value) | Out-Null }
    }
    # Leading comma keeps PowerShell from turning the set into an array,
    # which would make Contains a slow linear search.
    return ,$names
}

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

Write-Step 'Preflight'

if (-not $PSScriptRoot) { throw 'This script must be run from a file, not pasted into a shell.' }
if ($ExpectedMajor -and $SupportedMajors -notcontains $ExpectedMajor) {
    throw "-ExpectedMajor must be one of $($SupportedMajors -join ', ')."
}
$RepoRoot = (Split-Path -Parent $PSScriptRoot).TrimEnd('\')

$makefile = Join-Path $RepoRoot 'Makefile'
$controlFile = Join-Path $RepoRoot 'pg_textsearch.control'
$srcDir = Join-Path $RepoRoot 'src'
foreach ($required in @($makefile, $controlFile, $srcDir)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "$RepoRoot does not look like a pg_textsearch checkout: missing $required"
    }
}

$version = ([regex]::Match((Get-Content -LiteralPath $controlFile -Raw),
    "default_version\s*=\s*'([^']+)'")).Groups[1].Value
if (-not $version) {
    throw "Could not parse default_version from $controlFile"
}
$installSql = Join-Path $RepoRoot "sql\pg_textsearch--$version.sql"
if (-not (Test-Path -LiteralPath $installSql)) {
    throw "Missing install script for version ${version}: $installSql"
}
Write-Detail "repository root: $RepoRoot"
Write-Detail "pg_textsearch version: $version"

if (-not $PgRoot) {
    throw 'Specify -PgRoot: the root of a PostgreSQL 17 or 18 x64 tree (containing include\server and lib\postgres.lib).'
}
# Relative -PgRoot/-WorkDir follow the caller's location; the repository root
# never does, it comes from $PSScriptRoot alone.
$callerDir = (Get-Location).ProviderPath
$PgRoot = (Get-FullPath -Path $PgRoot -BaseDir $callerDir).TrimEnd('\')
$pgInclude = Join-Path $PgRoot 'include\server'
$pgLib = Join-Path $PgRoot 'lib\postgres.lib'
foreach ($required in @($pgInclude, $pgLib)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "PgRoot does not look like a PostgreSQL tree: missing $required"
    }
}

if ($WorkDir) {
    $WorkDir = (Get-FullPath -Path $WorkDir -BaseDir $callerDir).TrimEnd('\')
}
else {
    $WorkDir = (Join-Path $RepoRoot 'target\msvc').TrimEnd('\')
}

# Keep PostgreSQL inputs and build outputs disjoint. This makes cleanup safe
# even if the scratch layout changes later.
$pgRootLower = $PgRoot.ToLowerInvariant()
$workDirLower = $WorkDir.ToLowerInvariant()
if ($workDirLower -eq $pgRootLower -or $workDirLower.StartsWith($pgRootLower + '\')) {
    throw 'WorkDir must not be inside PgRoot. This script never writes into the PostgreSQL tree.'
}
if ($pgRootLower.StartsWith($workDirLower + '\')) {
    throw 'PgRoot must not be inside WorkDir. The PostgreSQL tree and build output must not overlap.'
}

$pgConfigH = Join-Path $pgInclude 'pg_config.h'
$versionLine = Select-String -LiteralPath $pgConfigH -Pattern '^#define\s+PG_VERSION\s+"' |
    Select-Object -First 1
if (-not $versionLine) {
    throw "Could not read PG_VERSION from $pgConfigH"
}
$pgVersion = ([regex]::Match($versionLine.Line, '"([^"]+)"')).Groups[1].Value
$pgMajor = ([regex]::Match($pgVersion, '^(\d+)')).Groups[1].Value
Write-Detail "PostgreSQL headers report version $pgVersion"
if ($ExpectedMajor) {
    if ($pgMajor -ne $ExpectedMajor) {
        throw "PgRoot is PostgreSQL $pgVersion but -ExpectedMajor $ExpectedMajor was requested."
    }
}
elseif ($SupportedMajors -notcontains $pgMajor) {
    throw "PgRoot is PostgreSQL $pgVersion; this script supports $($SupportedMajors -join ' and ')."
}

$vcvars = Find-VcVars
Write-Detail "vcvars64.bat: $vcvars"
Import-VcVarsEnvironment $vcvars
if ($env:VCToolsVersion) { Write-Detail "MSVC toolset: $env:VCToolsVersion" }
if ($env:VSCMD_VER) { Write-Detail "Visual Studio: $env:VSCMD_VER" }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'cl.exe is not on PATH after running vcvars64.bat'
}
if (-not (Get-Command link.exe -ErrorAction SilentlyContinue)) {
    throw 'link.exe is not on PATH after running vcvars64.bat'
}

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

Write-Step 'Sources'

# Group by directory: three different directories contain a scan.c, so one
# flat object directory would silently drop objects.
$sourceGroups = [ordered]@{}
$layoutCheckRelative = 'src\layout_check.c'
$haveLayoutCheck = $false
$sourceCount = 0
foreach ($obj in (Get-MakefileVariable -MakefilePath $makefile -Name 'OBJS')) {
    if ($obj -notmatch '\.o$') { throw "Unexpected OBJS entry in ${makefile}: $obj" }
    $relative = ($obj -replace '\.o$', '.c') -replace '/', '\'
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $relative))) {
        throw "Makefile OBJS lists $obj but $relative does not exist"
    }
    if ($relative -eq $layoutCheckRelative) { $haveLayoutCheck = $true }
    $dir = Split-Path -Parent $relative
    if (-not $sourceGroups.Contains($dir)) {
        $sourceGroups[$dir] = New-Object System.Collections.Generic.List[string]
    }
    $sourceGroups[$dir].Add($relative)
    $sourceCount++
}
if (-not $haveLayoutCheck) {
    throw "Makefile OBJS no longer lists $layoutCheckRelative; the struct layout gate would be silently gone."
}
Write-Detail "$sourceCount sources in $($sourceGroups.Count) directories, from the Makefile OBJS list"

$dataFiles = Get-MakefileVariable -MakefilePath $makefile -Name 'DATA'
Write-Detail "$($dataFiles.Count) SQL files from the Makefile DATA list"

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------

Write-Step 'Compile'
$dll = Join-Path $WorkDir 'pg_textsearch.dll'
if (Test-Path -LiteralPath $dll) {
    Remove-Item -LiteralPath $dll -Force
}

$objRoot = Join-Path $WorkDir 'obj'
$rspDir = Join-Path $WorkDir 'rsp'
New-CleanDirectory $objRoot
New-CleanDirectory $rspDir

# Same include paths as the Makefile (-I$(srcdir)/src) plus the PostgreSQL
# tree; no source includes "src/...", so no repository-root /I is needed.
$flags = @($CompileFlags)
$flags += $CompileDefines
# \" puts quotes into the /D value so PG_TEXTSEARCH_VERSION becomes a quoted
# string, same as -DPG_TEXTSEARCH_VERSION=\"...\" in the GCC build.
$flags += ('/DPG_TEXTSEARCH_VERSION=\"' + $version + '\"')
$flags += ('/I"' + $srcDir + '"')
foreach ($include in @('include\server\port\win32_msvc', 'include\server\port\win32', 'include\server', 'include')) {
    $flags += ('/I"' + (Join-Path $PgRoot $include) + '"')
}

$objectFiles = New-Object System.Collections.Generic.List[string]

# Compile layout_check.c first and alone: a layout failure then stands alone
# instead of mixing with errors from every other translation unit.
Write-Detail "layout gate: compiling $layoutCheckRelative"
try {
    Invoke-Cl -Tag 'layout-check' -RspDir $rspDir -ObjDir (Join-Path $objRoot 'src') -Flags $flags `
        -Sources @((Join-Path $RepoRoot $layoutCheckRelative))
}
catch {
    Write-Host ''
    Write-Host 'LAYOUT GATE: FAIL' -ForegroundColor Red
    Write-Host 'The layout-check translation unit did not compile; inspect the compiler diagnostics above.' -ForegroundColor Red
    Write-Host 'If a static layout assertion failed, this build cannot safely read existing index data.' -ForegroundColor Red
    throw
}
Write-Host '    LAYOUT GATE: PASS' -ForegroundColor Green
$objectFiles.Add((Join-Path $objRoot ($layoutCheckRelative -replace '\.c$', '.obj')))

foreach ($dir in $sourceGroups.Keys) {
    $relatives = @($sourceGroups[$dir] | Where-Object { $_ -ne $layoutCheckRelative })
    if ($relatives.Count -eq 0) { continue }
    Write-Detail "compiling $($dir -replace '\\', '/') ($($relatives.Count) files)"
    Invoke-Cl -Tag ($dir -replace '\\', '_') -RspDir $rspDir -ObjDir (Join-Path $objRoot $dir) `
        -Flags $flags -Sources @($relatives | ForEach-Object { Join-Path $RepoRoot $_ })
    foreach ($relative in $relatives) {
        $objectFiles.Add((Join-Path $objRoot ($relative -replace '\.c$', '.obj')))
    }
}

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------

Write-Step 'Link'

$linkLines = @(
    '/nologo'
    '/DLL'
    '/MACHINE:X64'
    ('/OUT:"' + $dll + '"')
    ('/IMPLIB:"' + (Join-Path $objRoot 'pg_textsearch.lib') + '"')
    ('"' + $pgLib + '"')
)
foreach ($obj in $objectFiles) {
    if (-not (Test-Path -LiteralPath $obj)) { throw "Missing object file: $obj" }
    $linkLines += ('"' + $obj + '"')
}
Write-TextFile -Path (Join-Path $rspDir 'link.rsp') -Lines $linkLines
Push-Location -LiteralPath $rspDir
try {
    & link.exe '@link.rsp'
    if ($LASTEXITCODE -ne 0) { throw "link.exe failed (exit code $LASTEXITCODE)" }
}
finally {
    Pop-Location
}
Write-Detail "linked $($objectFiles.Count) objects into $dll"

# ---------------------------------------------------------------------------
# Export gate
# ---------------------------------------------------------------------------

Write-Step 'Export gate'

# Every DATA file, not just the install script: tp_memory_usage is only
# bound by an upgrade script and would otherwise go unchecked.
$wanted = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
foreach ($data in $dataFiles) {
    $path = Join-Path $RepoRoot ($data -replace '/', '\')
    if (-not (Test-Path -LiteralPath $path)) { throw "Makefile DATA lists $data but it does not exist" }
    $text = Get-Content -LiteralPath $path -Raw
    $matched = [regex]::Matches($text, "MODULE_PATHNAME'\s*,\s*'([A-Za-z0-9_]+)'")
    # The one-argument AS 'MODULE_PATHNAME' form would name the symbol after
    # the SQL function and escape the regex, so count the mentions too.
    $mentions = ([regex]::Matches($text, 'MODULE_PATHNAME')).Count
    if ($mentions -ne $matched.Count) {
        throw "$data has $mentions MODULE_PATHNAME mentions but $($matched.Count) parse as explicit symbol references; the export gate would miss $($mentions - $matched.Count) of them."
    }
    foreach ($match in $matched) { $wanted.Add($match.Groups[1].Value) | Out-Null }
}
if ($wanted.Count -eq 0) { throw 'Found no MODULE_PATHNAME symbol references in the DATA SQL files' }

# Case-sensitive comparison: GetProcAddress matches case exactly, while
# PowerShell string comparison ignores case by default.
$exported = Get-DllExport -DllPath $dll
$missing = @($wanted | Where-Object { -not $exported.Contains($_) } | Sort-Object)
if ($missing.Count -gt 0) {
    Write-Host ''
    Write-Host 'EXPORT GATE: FAIL' -ForegroundColor Red
    Write-Host "Not exported by the DLL: $($missing -join ', ')" -ForegroundColor Red
    throw 'The built DLL does not export every symbol the SQL scripts bind. A PGDLLEXPORT is probably missing.'
}
Write-Host "    EXPORT GATE: PASS ($($wanted.Count) SQL symbols, $($exported.Count) exports in the DLL)" -ForegroundColor Green

Write-Step 'Done'
Write-Detail ("{0} ({1:N0} bytes)" -f $dll, (Get-Item -LiteralPath $dll).Length)
Write-Detail 'Nothing was written into the PostgreSQL tree.'
