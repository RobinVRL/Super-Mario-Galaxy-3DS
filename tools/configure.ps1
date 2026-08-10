[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Dol,
    [ValidatePattern('^[A-Za-z0-9]{6}$')]
    [string]$TitleId = 'RMGE01',
    [string]$Map,
    [ValidateRange(0, 1024)]
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
$dolPath = (Resolve-Path -LiteralPath $Dol).Path
$mapPath = if ($Map) { (Resolve-Path -LiteralPath $Map).Path } else { $null }
$originalPath = $env:PATH

function Read-BeUInt32([byte[]]$Bytes, [int]$Offset) {
    if ($Offset -lt 0 -or ($Offset + 4) -gt $Bytes.Length) {
        throw "Big-endian read is outside the input at offset 0x$($Offset.ToString('X'))."
    }

    return [uint32](
        ([uint64]$Bytes[$Offset] -shl 24) -bor
        ([uint64]$Bytes[$Offset + 1] -shl 16) -bor
        ([uint64]$Bytes[$Offset + 2] -shl 8) -bor
        [uint64]$Bytes[$Offset + 3]
    )
}

function Test-IsReparsePoint([IO.FileSystemInfo]$Item) {
    return (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Assert-NotReparsePoint([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (Test-IsReparsePoint $item) {
        throw "Refusing to use reparse point: $($item.FullName)"
    }
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

function Assert-SafeTransactionPath(
    [string]$Path,
    [string]$Parent,
    [string]$LeafPattern
) {
    $full = [IO.Path]::GetFullPath($Path)
    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    $boundary = $parentFull + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Transaction path is outside its expected parent: $full"
    }

    $relative = $full.Substring($boundary.Length)
    if ($relative.Contains([IO.Path]::DirectorySeparatorChar) -or
        $relative.Contains([IO.Path]::AltDirectorySeparatorChar) -or
        $relative -notmatch $LeafPattern) {
        throw "Unexpected transaction path: $full"
    }

    if (Test-Path -LiteralPath $full) {
        $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
        if (-not $item.PSIsContainer) {
            throw "Transaction path is not a directory: $full"
        }
        Assert-NoReparsePoints $full
    }

    return $full
}

function Remove-SafeTransactionTree(
    [string]$Path,
    [string]$Parent,
    [string]$LeafPattern
) {
    $full = Assert-SafeTransactionPath -Path $Path -Parent $Parent `
        -LeafPattern $LeafPattern
    if (-not (Test-Path -LiteralPath $full)) {
        return
    }

    $items = @(Get-ChildItem -LiteralPath $full -Force -Recurse)
    Write-Verbose "Removing validated transaction tree with $($items.Count) descendants: $full"
    Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop
}

function Assert-ValidDol([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (-not $item.PSIsContainer -and $item.Length -lt 1MB) {
        throw "DOL is only $($item.Length) bytes; expected the retail game DOL (at least 1 MiB), not an update fixture."
    }
    if ($item.PSIsContainer) {
        throw "DOL path is a directory: $Path"
    }
    Assert-NoReparsePoints $Path

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x100) {
        throw 'DOL is smaller than its 0x100-byte header.'
    }

    $sections = @()
    for ($index = 0; $index -lt 7; $index++) {
        $sections += [pscustomobject]@{
            Kind = 'text'
            Index = $index
            FileOffset = Read-BeUInt32 $bytes (0x00 + 4 * $index)
            Address = Read-BeUInt32 $bytes (0x48 + 4 * $index)
            Size = Read-BeUInt32 $bytes (0x90 + 4 * $index)
        }
    }
    for ($index = 0; $index -lt 11; $index++) {
        $sections += [pscustomobject]@{
            Kind = 'data'
            Index = $index
            FileOffset = Read-BeUInt32 $bytes (0x1C + 4 * $index)
            Address = Read-BeUInt32 $bytes (0x64 + 4 * $index)
            Size = Read-BeUInt32 $bytes (0xAC + 4 * $index)
        }
    }

    $loaded = @($sections | Where-Object { $_.Size -ne 0 })
    if ($loaded.Count -eq 0) {
        throw 'DOL has no loadable sections.'
    }

    [uint64]$loadedBytes = 0
    foreach ($section in $loaded) {
        [uint64]$fileStart = $section.FileOffset
        [uint64]$fileEnd = $fileStart + $section.Size
        [uint64]$memoryStart = $section.Address
        [uint64]$memoryEnd = $memoryStart + $section.Size
        if ($fileStart -lt 0x100 -or $fileEnd -gt [uint64]$bytes.Length) {
            throw "DOL $($section.Kind) section $($section.Index) is outside the file."
        }
        if ($memoryStart -lt 0x80000000L -or $memoryEnd -gt 0x94000000L) {
            throw "DOL $($section.Kind) section $($section.Index) has an invalid Wii load range."
        }
        $loadedBytes += $section.Size
    }
    if ($loadedBytes -lt 1MB) {
        throw "DOL contains only $loadedBytes bytes of loadable data; expected the retail game executable."
    }

    $fileOrdered = @($loaded | Sort-Object FileOffset)
    for ($index = 1; $index -lt $fileOrdered.Count; $index++) {
        [uint64]$previousEnd = [uint64]$fileOrdered[$index - 1].FileOffset +
            [uint64]$fileOrdered[$index - 1].Size
        if ($previousEnd -gt [uint64]$fileOrdered[$index].FileOffset) {
            throw 'DOL loadable sections overlap in the input file.'
        }
    }

    $entry = Read-BeUInt32 $bytes 0xE0
    $entrySection = @($loaded | Where-Object {
        $_.Kind -eq 'text' -and
        [uint64]$entry -ge [uint64]$_.Address -and
        [uint64]$entry -lt ([uint64]$_.Address + [uint64]$_.Size)
    })
    if ($entrySection.Count -ne 1) {
        throw "DOL entry point 0x$($entry.ToString('X8')) is not inside one executable text section."
    }

    $bssAddress = Read-BeUInt32 $bytes 0xD8
    $bssSize = Read-BeUInt32 $bytes 0xDC
    if ($bssSize -ne 0) {
        [uint64]$bssEnd = [uint64]$bssAddress + [uint64]$bssSize
        if ([uint64]$bssAddress -lt 0x80000000L -or $bssEnd -gt 0x94000000L) {
            throw 'DOL BSS range is outside Wii memory.'
        }
    }

    return [pscustomobject]@{
        Entry = $entry
        SectionCount = $loaded.Count
        LoadedBytes = $loadedBytes
        FileBytes = $bytes.Length
    }
}

function Assert-ValidDiscMetadata([string]$BootPath, [string]$FstPath) {
    foreach ($path in @($BootPath, $FstPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Required disc metadata is missing beside main.dol: $path"
        }
        Assert-NoReparsePoints $path
    }

    $bootItem = Get-Item -LiteralPath $BootPath
    if ($bootItem.Length -lt 0x440) {
        throw "boot.bin is truncated ($($bootItem.Length) bytes)."
    }

    $fstBytes = [IO.File]::ReadAllBytes($FstPath)
    if ($fstBytes.Length -lt 12) {
        throw 'fst.bin is smaller than its root entry.'
    }
    $rootWord = Read-BeUInt32 $fstBytes 0
    if (($rootWord -band 0xFF000000) -ne 0x01000000) {
        throw 'fst.bin root entry is not a directory.'
    }
    $entryCount = Read-BeUInt32 $fstBytes 8
    if ($entryCount -lt 1 -or ([uint64]$entryCount * 12) -gt [uint64]$fstBytes.Length) {
        throw "fst.bin has an invalid entry count: $entryCount"
    }
}

function Assert-CoherentGeneratedOutput([string]$Directory) {
    Assert-NoReparsePoints $Directory
    $generatedC = Join-Path $Directory 'generated.c'
    $generatedH = Join-Path $Directory 'generated.h'
    $chunkDirectory = Join-Path $Directory 'generated_chunks'
    foreach ($path in @($generatedC, $generatedH)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "DolRecomp did not create required output: $path"
        }
        if ((Get-Item -LiteralPath $path).Length -eq 0) {
            throw "DolRecomp created an empty output file: $path"
        }
    }
    if (-not (Test-Path -LiteralPath $chunkDirectory -PathType Container)) {
        throw 'DolRecomp did not create generated_chunks.'
    }
    Assert-NoReparsePoints $chunkDirectory

    $listed = @()
    $declaredCounts = @()
    foreach ($line in [IO.File]::ReadAllLines($generatedC)) {
        if ($line -match '^// generated_chunks/(?<name>[^/\\]+\.c)$') {
            $listed += $Matches.name
        }
        elseif ($line -match '^// (?<count>[0-9]+) C files$') {
            $declaredCounts += [int]$Matches.count
        }
    }
    $actual = @(Get-ChildItem -LiteralPath $chunkDirectory -Filter '*.c' -File |
        Sort-Object Name | ForEach-Object { $_.Name })
    $listedSorted = @($listed | Sort-Object)

    if ($actual.Count -eq 0 -or $listed.Count -eq 0) {
        throw 'DolRecomp produced no split C sources.'
    }
    if (@($listed | Sort-Object -Unique).Count -ne $listed.Count) {
        throw 'generated.c lists a split source more than once.'
    }
    $difference = @(Compare-Object -ReferenceObject $listedSorted -DifferenceObject $actual)
    if ($difference.Count -ne 0) {
        throw 'generated.c and generated_chunks contain different source inventories.'
    }
    if ($declaredCounts.Count -ne 1 -or $declaredCounts[0] -ne $actual.Count) {
        throw 'generated.c split-source count does not match generated_chunks.'
    }
}

function Copy-VerifiedFile([string]$Source, [string]$Destination) {
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    $sourceItem = Get-Item -LiteralPath $Source
    $destinationItem = Get-Item -LiteralPath $Destination
    if ($sourceItem.Length -ne $destinationItem.Length) {
        throw "Staged copy has the wrong size: $Destination"
    }
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($sourceHash -ne $destinationHash) {
        throw "Staged copy failed verification: $Destination"
    }
}

$dolInfo = Assert-ValidDol $dolPath
$petariManifestCheck = Join-Path $PSScriptRoot 'verify-petari-manifest.ps1'
& $petariManifestCheck -Dol $dolPath
$sysDirectory = Split-Path -Parent $dolPath
$bootPath = Join-Path $sysDirectory 'boot.bin'
$fstPath = Join-Path $sysDirectory 'fst.bin'
Assert-ValidDiscMetadata -BootPath $bootPath -FstPath $fstPath

$build = Join-Path $root 'external\DolRecomp\build-host'
$toolCandidates = @(
    (Join-Path $build 'dolrecomp.exe'),
    (Join-Path $build 'Release\dolrecomp.exe'),
    (Join-Path $root 'external\DolRecomp\build\dolrecomp.exe'),
    (Join-Path $root 'external\DolRecomp\build\Release\dolrecomp.exe')
)
$tool = $toolCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if (-not $tool) {
    & cmake -S (Join-Path $root 'external\DolRecomp') -B $build
    if ($LASTEXITCODE -ne 0) {
        throw 'CMake could not configure DolRecomp. Install a host C compiler.'
    }
    & cmake --build $build --config Release
    if ($LASTEXITCODE -ne 0) {
        throw 'DolRecomp failed to build.'
    }
    $tool = $toolCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}
if (-not $tool) {
    throw 'DolRecomp built successfully but dolrecomp.exe was not found.'
}
Assert-NoReparsePoints $tool

$gcc = Get-Command gcc.exe -ErrorAction SilentlyContinue
$mingwBin = if ($gcc) { Split-Path -Parent $gcc.Source } else { 'C:\msys64\mingw32\bin' }
if (Test-Path -LiteralPath $mingwBin -PathType Container) {
    $env:PATH = "$mingwBin;$env:PATH"
}

$transactionId = [Guid]::NewGuid().ToString('N')
$generated = Join-Path $root 'generated'
$generatedStage = Join-Path $root "generated.stage-$transactionId"
$generatedBackup = Join-Path $root "generated.backup-$transactionId"
$romfs = Join-Path $root 'romfs'
$game = Join-Path $romfs 'game'
$gameStage = Join-Path $romfs "game.stage-$transactionId"
$gameBackup = Join-Path $romfs "game.backup-$transactionId"
$generatedPattern = '^generated\.(?:stage|backup)-[0-9a-f]{32}$'
$gamePattern = '^game\.(?:stage|backup)-[0-9a-f]{32}$'

Assert-NotReparsePoint $root
Assert-NotReparsePoint $romfs
foreach ($candidate in @($generatedStage, $generatedBackup)) {
    Assert-SafeTransactionPath -Path $candidate -Parent $root `
        -LeafPattern $generatedPattern | Out-Null
}
foreach ($candidate in @($gameStage, $gameBackup)) {
    Assert-SafeTransactionPath -Path $candidate -Parent $romfs `
        -LeafPattern $gamePattern | Out-Null
}

$generatedInstalled = $false
$gameInstalled = $false
$generatedBackedUp = $false
$gameBackedUp = $false

try {
    New-Item -ItemType Directory -Path $generatedStage -ErrorAction Stop | Out-Null
    $arguments = @('--cpu', 'broadway')
    if ($Jobs -gt 0) {
        $arguments += "-j$Jobs"
    }
    if ($mapPath) {
        $arguments += '--map', $mapPath
    }
    $arguments += $dolPath, $TitleId, (Join-Path $generatedStage 'generated.c')

    & $tool @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "DolRecomp failed with exit code $LASTEXITCODE"
    }
    Assert-CoherentGeneratedOutput $generatedStage

    $generatedKeep = Join-Path $generated '.gitkeep'
    if (Test-Path -LiteralPath $generatedKeep -PathType Leaf) {
        Copy-Item -LiteralPath $generatedKeep -Destination $generatedStage
    }

    New-Item -ItemType Directory -Path $gameStage -ErrorAction Stop | Out-Null
    Copy-VerifiedFile -Source $dolPath -Destination (Join-Path $gameStage 'main.dol')
    Copy-VerifiedFile -Source $bootPath -Destination (Join-Path $gameStage 'boot.bin')
    Copy-VerifiedFile -Source $fstPath -Destination (Join-Path $gameStage 'fst.bin')
    $gameKeep = Join-Path $game '.gitkeep'
    if (Test-Path -LiteralPath $gameKeep -PathType Leaf) {
        Copy-Item -LiteralPath $gameKeep -Destination $gameStage
    }

    try {
        if (Test-Path -LiteralPath $generated) {
            Assert-NoReparsePoints $generated
            Move-Item -LiteralPath $generated -Destination $generatedBackup -ErrorAction Stop
            $generatedBackedUp = $true
        }
        if (Test-Path -LiteralPath $game) {
            Assert-NoReparsePoints $game
            Move-Item -LiteralPath $game -Destination $gameBackup -ErrorAction Stop
            $gameBackedUp = $true
        }

        Move-Item -LiteralPath $generatedStage -Destination $generated -ErrorAction Stop
        $generatedInstalled = $true
        Move-Item -LiteralPath $gameStage -Destination $game -ErrorAction Stop
        $gameInstalled = $true
    }
    catch {
        $commitError = $_
        try {
            if ($gameInstalled) {
                Move-Item -LiteralPath $game -Destination $gameStage -ErrorAction Stop
                $gameInstalled = $false
            }
            if ($generatedInstalled) {
                Move-Item -LiteralPath $generated -Destination $generatedStage -ErrorAction Stop
                $generatedInstalled = $false
            }
            if ($gameBackedUp) {
                Move-Item -LiteralPath $gameBackup -Destination $game -ErrorAction Stop
                $gameBackedUp = $false
            }
            if ($generatedBackedUp) {
                Move-Item -LiteralPath $generatedBackup -Destination $generated -ErrorAction Stop
                $generatedBackedUp = $false
            }
        }
        catch {
            throw "Configuration commit failed and rollback also failed. Commit error: $commitError Rollback error: $_"
        }
        throw $commitError
    }

    if ($generatedBackedUp) {
        Remove-SafeTransactionTree -Path $generatedBackup -Parent $root `
            -LeafPattern $generatedPattern
        $generatedBackedUp = $false
    }
    if ($gameBackedUp) {
        Remove-SafeTransactionTree -Path $gameBackup -Parent $romfs `
            -LeafPattern $gamePattern
        $gameBackedUp = $false
    }

    Write-Host ("Configured retail DOL: {0} sections, {1} loaded bytes, entry 0x{2:X8}." -f `
        $dolInfo.SectionCount, $dolInfo.LoadedBytes, $dolInfo.Entry)
    Write-Host 'Generated code and RomFS disc metadata were replaced transactionally.'
}
finally {
    $env:PATH = $originalPath
    if (Test-Path -LiteralPath $generatedStage) {
        Remove-SafeTransactionTree -Path $generatedStage -Parent $root `
            -LeafPattern $generatedPattern
    }
    if (Test-Path -LiteralPath $gameStage) {
        Remove-SafeTransactionTree -Path $gameStage -Parent $romfs `
            -LeafPattern $gamePattern
    }
}
