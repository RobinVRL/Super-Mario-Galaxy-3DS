[CmdletBinding()]
param(
    [string]$DataRoot,
    [string]$AzaharSdmc
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path

if ([string]::IsNullOrWhiteSpace($DataRoot)) {
    $DataRoot = Join-Path $projectRoot 'extracted\DATA'
}
if ([string]::IsNullOrWhiteSpace($AzaharSdmc)) {
    $AzaharSdmc = Join-Path ([Environment]::GetFolderPath('ApplicationData')) `
        'Azahar\sdmc'
}

$source = (Resolve-Path -LiteralPath $DataRoot).Path
$required = @(
    (Join-Path $source 'sys\main.dol'),
    (Join-Path $source 'sys\boot.bin'),
    (Join-Path $source 'sys\fst.bin'),
    (Join-Path $source 'files')
)
foreach ($path in $required) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Extracted DATA root is incomplete; missing: $path"
    }
}

$sdmc = [IO.Path]::GetFullPath($AzaharSdmc)
New-Item -ItemType Directory -Path $sdmc -Force | Out-Null
$container = Join-Path $sdmc 'smg3ds'
New-Item -ItemType Directory -Path $container -Force | Out-Null
$target = [IO.Path]::GetFullPath((Join-Path $container 'DATA'))
$boundary = $sdmc.TrimEnd('\') + '\'
if (-not $target.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to create an asset link outside the selected Azahar SD root: $target"
}

if (Test-Path -LiteralPath $target) {
    $item = Get-Item -LiteralPath $target -Force
    $linkTargets = @($item.Target)
    $matchesSource = $false
    foreach ($linkTarget in $linkTargets) {
        if (-not [string]::IsNullOrWhiteSpace($linkTarget)) {
            $resolvedTarget = [IO.Path]::GetFullPath($linkTarget)
            if ($resolvedTarget.Equals($source,
                    [StringComparison]::OrdinalIgnoreCase)) {
                $matchesSource = $true
            }
        }
    }
    if ($item.LinkType -eq 'Junction' -and $matchesSource) {
        Write-Host "Azahar asset junction is already correct: $target -> $source"
        exit 0
    }
    throw "Refusing to replace existing Azahar asset path: $target"
}

New-Item -ItemType Junction -Path $target -Target $source | Out-Null
Write-Host "Azahar asset junction created: $target -> $source"
