[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Dol
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
$dolPath = (Resolve-Path -LiteralPath $Dol).Path
$manifestPath = Join-Path $root 'include\smg3ds\petari_overrides.h'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Petari override manifest is missing: $manifestPath"
}

$manifest = [IO.File]::ReadAllText($manifestPath)
$enabled = [regex]::Matches(
    $manifest,
    '(?m)^#define\s+SMG3DS_PETARI_[A-Z0-9_]+_ENABLED\s+1\s*$')
if ($enabled.Count -eq 0) {
    Write-Verbose 'Petari override manifest has no enabled entries.'
    return
}

$gameMatch = [regex]::Match(
    $manifest,
    '(?m)^#define\s+SMG3DS_PETARI_GAME_ID\s+"(?<id>[A-Za-z0-9]{6})"\s*$')
$hashMatch = [regex]::Match(
    $manifest,
    '(?ms)^#define\s+SMG3DS_PETARI_DOL_SHA256\s+\\\s*"(?<hash>[0-9A-Fa-f]{64})"\s*$')
if (-not $gameMatch.Success -or -not $hashMatch.Success) {
    throw 'Enabled Petari overrides require a six-character game ID and DOL SHA-256.'
}

$expectedHash = $hashMatch.Groups['hash'].Value.ToUpperInvariant()
$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dolPath).Hash
if ($actualHash -ne $expectedHash) {
    throw ("Petari overrides are enabled for {0} DOL SHA-256 {1}, but the " +
        "selected DOL is {2}. Disable the overrides or add a separately " +
        "verified revision manifest.") -f
        $gameMatch.Groups['id'].Value, $expectedHash, $actualHash
}

Write-Verbose ("Petari override manifest matches {0} ({1})." -f
    $gameMatch.Groups['id'].Value, $actualHash)
