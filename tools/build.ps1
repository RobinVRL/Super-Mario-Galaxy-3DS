[CmdletBinding()]
param(
    [ValidateRange(0, 1024)]
    [int]$Jobs = 0,
    [switch]$Incremental
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path

if ($Jobs -le 0) {
    $Jobs = [Math]::Max(24, [Environment]::ProcessorCount)
}
elseif ($Jobs -lt 24) {
    throw "At least 24 parallel jobs are required; requested: $Jobs"
}

# Validate the retail DOL, generated inventory, disc assets, rasterizer pixel
# proof, EXI transactions, and exception-vector handoff before staging or
# compiling anything.
$renderPreflight = Join-Path $PSScriptRoot 'verify-render-path.ps1'
if (-not (Test-Path -LiteralPath $renderPreflight -PathType Leaf)) {
    throw "Render-path preflight is missing: $renderPreflight"
}
& $renderPreflight

if (-not $env:DEVKITPRO) {
    throw 'DEVKITPRO is not set.'
}

function Convert-ToWindowsPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'Expected a non-empty path.'
    }

    if ($Path -match '^/([A-Za-z])(?:/(.*))?$') {
        $drive = $Matches[1].ToUpperInvariant()
        $rest = if ($Matches[2]) { $Matches[2] -replace '/', '\' } else { '' }
        return [IO.Path]::GetFullPath("${drive}:\$rest")
    }

    return [IO.Path]::GetFullPath($Path)
}

function Convert-ToMsysPath([string]$Path) {
    $full = Convert-ToWindowsPath $Path
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "Expected an absolute Windows path, got: $Path"
    }

    $drive = $Matches[1].ToLowerInvariant()
    $rest = ($Matches[2] -replace '\\', '/').TrimStart('/')
    if ([string]::IsNullOrWhiteSpace($rest)) {
        return "/$drive"
    }
    return "/$drive/$rest"
}

function Test-IsReparsePoint([IO.FileSystemInfo]$Item) {
    return (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Assert-NoReparsePoints([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (Test-IsReparsePoint $item) {
        throw "Refusing to use reparse point: $($item.FullName)"
    }

    if ($item.PSIsContainer) {
        $pending = [Collections.Generic.Stack[string]]::new()
        $pending.Push($item.FullName)
        while ($pending.Count -gt 0) {
            $directory = $pending.Pop()
            foreach ($child in @(Get-ChildItem -LiteralPath $directory -Force)) {
                if (Test-IsReparsePoint $child) {
                    throw "Refusing to traverse reparse point: $($child.FullName)"
                }
                if ($child.PSIsContainer) {
                    $pending.Push($child.FullName)
                }
            }
        }
    }
}

function Assert-SafeStagePath([string]$Path, [string]$TempRoot) {
    $full = [IO.Path]::GetFullPath($Path)
    $temp = [IO.Path]::GetFullPath($TempRoot).TrimEnd('\', '/')
    $boundary = $temp + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Staging path is outside the temporary root: $full"
    }

    $relative = $full.Substring($boundary.Length)
    if ($relative.Contains([IO.Path]::DirectorySeparatorChar) -or
        $relative.Contains([IO.Path]::AltDirectorySeparatorChar) -or
        $relative -notmatch '^smg3ds-build-(?:run-[0-9a-f]{32}|cache-[0-9a-f]{16})$') {
        throw "Unexpected staging path: $full"
    }

    if (Test-Path -LiteralPath $full) {
        $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
        if (-not $item.PSIsContainer) {
            throw "Staging path is not a directory: $full"
        }
        Assert-NoReparsePoints $full
    }

    return $full
}

function Remove-SafeStage([string]$Path, [string]$TempRoot) {
    $full = Assert-SafeStagePath -Path $Path -TempRoot $TempRoot
    if (-not (Test-Path -LiteralPath $full)) {
        return
    }

    $items = @(Get-ChildItem -LiteralPath $full -Force -Recurse)
    Write-Verbose "Removing validated staging tree with $($items.Count) descendants: $full"
    Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop
}

function Sync-Directory([string]$Source, [string]$Destination, [string]$Robocopy) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Required build input is missing: $Source"
    }
    Assert-NoReparsePoints $Source

    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    }
    Assert-NoReparsePoints $Destination

    & $Robocopy $Source $Destination /MIR /COPY:DAT /DCOPY:DAT /R:1 /W:1 /XJ `
        /NFL /NDL /NJH /NJS /NP
    $robocopyExit = $LASTEXITCODE
    if ($robocopyExit -gt 7) {
        throw "Could not stage '$Source' (robocopy exit code $robocopyExit)."
    }
}

function Get-RootKey([string]$Path) {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Path.ToLowerInvariant())
        $hash = $sha256.ComputeHash($bytes)
        return -join ($hash[0..7] | ForEach-Object { $_.ToString('x2') })
    }
    finally {
        $sha256.Dispose()
    }
}

$devkitProWindows = Convert-ToWindowsPath $env:DEVKITPRO
$msysBin = Join-Path $devkitProWindows 'msys2\usr\bin'
$make = Join-Path $msysBin 'make.exe'
if (-not (Test-Path -LiteralPath $make -PathType Leaf)) {
    throw "GNU make was not found at: $make"
}

$robocopyCommand = Get-Command robocopy.exe -ErrorAction Stop
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
if ($Incremental) {
    $stageName = 'smg3ds-build-cache-' + (Get-RootKey $root)
}
else {
    $stageName = 'smg3ds-build-run-' + [Guid]::NewGuid().ToString('N')
}
$stage = Assert-SafeStagePath -Path (Join-Path $tempRoot $stageName) -TempRoot $tempRoot
$removeStage = -not $Incremental

if (-not (Test-Path -LiteralPath $stage)) {
    New-Item -ItemType Directory -Path $stage -ErrorAction Stop | Out-Null
}
Assert-SafeStagePath -Path $stage -TempRoot $tempRoot | Out-Null

$originalDevkitPro = $env:DEVKITPRO
$originalDevkitArm = $env:DEVKITARM
$originalPath = $env:PATH

try {
    $makefileSource = Join-Path $root 'Makefile'
    Assert-NoReparsePoints $makefileSource
    $makefileDestination = Join-Path $stage 'Makefile'
    if (Test-Path -LiteralPath $makefileDestination) {
        Assert-NoReparsePoints $makefileDestination
    }
    Copy-Item -LiteralPath $makefileSource -Destination $makefileDestination -Force

    foreach ($relativeDirectory in @(
        'source',
        'include',
        'generated',
        'romfs',
        'external\DolRecomp\src'
    )) {
        $source = Join-Path $root $relativeDirectory
        $destination = Join-Path $stage $relativeDirectory
        Sync-Directory -Source $source -Destination $destination `
            -Robocopy $robocopyCommand.Source
    }

    $buildDir = Join-Path $stage 'build'
    if (-not (Test-Path -LiteralPath $buildDir)) {
        New-Item -ItemType Directory -Path $buildDir -ErrorAction Stop | Out-Null
    }
    Assert-NoReparsePoints $buildDir

    $env:DEVKITPRO = Convert-ToMsysPath $devkitProWindows
    $env:DEVKITARM = "$env:DEVKITPRO/devkitARM"
    $env:PATH = "$msysBin;$originalPath"

    Write-Host "Building in isolated staging tree: $stage"
    & $make '-C' $buildDir '-f' '../Makefile' 'TOPDIR=..' "-j$Jobs" 'smg3ds-bringup.elf'
    if ($LASTEXITCODE -ne 0) {
        throw "3DS ELF build failed with exit code $LASTEXITCODE"
    }

    # The generated translation carries enough DWARF data to push the ELF file
    # itself beyond 3dsxtool's 256 MiB input limit, even though the loadable
    # image is much smaller. Keep the linker map for diagnostics and discard
    # only debug sections before packaging.
    $strip = Join-Path $devkitProWindows 'devkitARM\bin\arm-none-eabi-strip.exe'
    if (-not (Test-Path -LiteralPath $strip -PathType Leaf)) {
        throw "ARM strip tool was not found at: $strip"
    }
    $elf = Join-Path $buildDir 'smg3ds-bringup.elf'
    & $strip '--strip-debug' $elf
    if ($LASTEXITCODE -ne 0) {
        throw "Could not strip 3DS ELF debug sections (exit code $LASTEXITCODE)"
    }

    & $make '-C' $buildDir '-f' '../Makefile' 'TOPDIR=..' "-j$Jobs" 'smg3ds-bringup.3dsx'
    if ($LASTEXITCODE -ne 0) {
        throw "3DS packaging failed with exit code $LASTEXITCODE"
    }

    foreach ($extension in @('3dsx', 'elf', 'map', 'smdh', 'lst')) {
        $artifact = Join-Path $buildDir "smg3ds-bringup.$extension"
        if (Test-Path -LiteralPath $artifact -PathType Leaf) {
            Copy-Item -LiteralPath $artifact -Destination $root -Force
        }
    }

    if ($Incremental) {
        Write-Host "Incremental object cache retained at: $buildDir"
    }
}
finally {
    $env:DEVKITPRO = $originalDevkitPro
    $env:DEVKITARM = $originalDevkitArm
    $env:PATH = $originalPath

    if ($removeStage) {
        Remove-SafeStage -Path $stage -TempRoot $tempRoot
    }
}
