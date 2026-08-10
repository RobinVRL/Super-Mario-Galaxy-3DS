[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
$chunks = Join-Path $root 'generated\generated_chunks'
$target = 'label_803E5934:'
$matches = @(Get-ChildItem -LiteralPath $chunks -File -Filter 'chunk_*.c' |
    Select-String -SimpleMatch $target -List)
if ($matches.Count -ne 1) {
    throw "Expected one generated fallback containing $target; found $($matches.Count)."
}

$gccPath = 'C:\msys64\mingw32\bin\gcc.exe'
if (-not (Test-Path -LiteralPath $gccPath -PathType Leaf)) {
    $gcc = Get-Command gcc.exe -ErrorAction Stop
    $gccPath = $gcc.Source
}
$mingwBin = Split-Path -Parent $gccPath
$env:PATH = "$mingwBin;C:\msys64\usr\bin;$env:PATH"

$outputDirectory = Join-Path $root 'external\DolRecomp\build-host'
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $outputDirectory -ErrorAction Stop |
        Out-Null
}
$output = Join-Path $outputDirectory 'petari_override_test.exe'
$arguments = @(
    '-std=gnu11', '-O2', '-Wall', '-Wextra',
    '-Wno-unused-label', '-Wno-unused-function',
    "-I$(Join-Path $root 'include')",
    "-I$(Join-Path $root 'external\DolRecomp\src')",
    '-include', (Join-Path $root 'include\smg3ds\petari_overrides.h'),
    (Join-Path $root 'tests\petari_override_test.c'),
    (Join-Path $root 'source\petari\petari_math_util.c'),
    (Join-Path $root 'source\petari\petari_cpu_bridge.c'),
    (Join-Path $root 'external\DolRecomp\src\cpu\cpu.c'),
    $matches[0].Path,
    '-lm', '-o', $output
)

& $gccPath @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Petari differential test did not compile (exit $LASTEXITCODE)."
}

& $output
if ($LASTEXITCODE -ne 0) {
    throw "Petari differential test failed (exit $LASTEXITCODE)."
}
