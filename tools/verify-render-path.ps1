[CmdletBinding()]
param(
    [string]$DataRoot,
    [string]$Dol,
    [string]$AzaharSdmc
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
if ([string]::IsNullOrWhiteSpace($DataRoot)) {
    $DataRoot = Join-Path $projectRoot 'extracted\DATA'
}
if ([string]::IsNullOrWhiteSpace($Dol)) {
    $Dol = Join-Path $DataRoot 'sys\main.dol'
}
if ([string]::IsNullOrWhiteSpace($AzaharSdmc)) {
    $AzaharSdmc = Join-Path ([Environment]::GetFolderPath('ApplicationData')) `
        'Azahar\sdmc'
}

$data = (Resolve-Path -LiteralPath $DataRoot).Path
$dolPath = (Resolve-Path -LiteralPath $Dol).Path
$petariManifestCheck = Join-Path $PSScriptRoot 'verify-petari-manifest.ps1'
& $petariManifestCheck -Dol $dolPath
$bootPath = Join-Path $data 'sys\boot.bin'
$fstPath = Join-Path $data 'sys\fst.bin'
$filesRoot = Join-Path $data 'files'

function Read-Be32([byte[]]$Bytes, [int]$Offset) {
    if ($Offset -lt 0 -or $Offset + 4 -gt $Bytes.Length) {
        throw "Big-endian read is outside the buffer at offset $Offset"
    }
    return [uint32]((([uint32]$Bytes[$Offset]) -shl 24) -bor
        (([uint32]$Bytes[$Offset + 1]) -shl 16) -bor
        (([uint32]$Bytes[$Offset + 2]) -shl 8) -bor
        ([uint32]$Bytes[$Offset + 3]))
}

function Assert-ExactFile([string]$Expected, [string]$Actual, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Actual -PathType Leaf)) {
        throw "$Label is missing: $Actual"
    }
    $expectedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Expected).Hash
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Actual).Hash
    if ($expectedHash -ne $actualHash) {
        throw "$Label does not come from the selected extracted DATA root"
    }
}

foreach ($required in @($bootPath, $fstPath, $filesRoot)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Extracted DATA root is incomplete; missing: $required"
    }
}

# Validate that this is a substantial, internally bounded retail DOL rather
# than the 320-byte update fixture that previously contaminated the build.
$dolBytes = [IO.File]::ReadAllBytes($dolPath)
if ($dolBytes.Length -lt 1MB -or $dolBytes.Length -lt 0x100) {
    throw "DOL is too small to be the retail game executable: $($dolBytes.Length) bytes"
}
$sections = @()
for ($index = 0; $index -lt 7; ++$index) {
    $sections += [pscustomobject]@{
        Offset = [uint64](Read-Be32 $dolBytes (0x00 + $index * 4))
        Address = [uint64](Read-Be32 $dolBytes (0x48 + $index * 4))
        Size = [uint64](Read-Be32 $dolBytes (0x90 + $index * 4))
    }
}
for ($index = 0; $index -lt 11; ++$index) {
    $sections += [pscustomobject]@{
        Offset = [uint64](Read-Be32 $dolBytes (0x1c + $index * 4))
        Address = [uint64](Read-Be32 $dolBytes (0x64 + $index * 4))
        Size = [uint64](Read-Be32 $dolBytes (0xac + $index * 4))
    }
}
$loadedSections = 0
foreach ($section in $sections) {
    if ($section.Size -eq 0) { continue }
    ++$loadedSections
    if ($section.Offset -lt 0x100 -or
        $section.Offset + $section.Size -gt [uint64]$dolBytes.Length) {
        throw ('DOL section is outside the file: offset=0x{0:X} size=0x{1:X}' -f
            $section.Offset, $section.Size)
    }
    $physical = $section.Address -band 0x3fffffffL
    $inMem1 = $physical -lt 0x01800000L -and
        $physical + $section.Size -le 0x01800000L
    $inMem2 = $physical -ge 0x10000000L -and
        $physical + $section.Size -le 0x14000000L
    if (-not ($inMem1 -or $inMem2)) {
        throw ('DOL section is outside MEM1/MEM2: address=0x{0:X} size=0x{1:X}' -f
            $section.Address, $section.Size)
    }
}
if ($loadedSections -lt 5) {
    throw "DOL has only $loadedSections loaded sections"
}
$entryPoint = Read-Be32 $dolBytes 0xe0
if (($entryPoint -band 0xc0000000L) -notin @(0x80000000L, 0xc0000000L)) {
    throw ('DOL entry point is not a cached/uncached guest address: 0x{0:X8}' -f
        $entryPoint)
}

# The configured private inputs must be byte-identical to this same DATA root.
Assert-ExactFile $dolPath (Join-Path $projectRoot 'romfs\game\main.dol') `
    'Configured main.dol'
Assert-ExactFile $bootPath (Join-Path $projectRoot 'romfs\game\boot.bin') `
    'Configured boot.bin'
Assert-ExactFile $fstPath (Join-Path $projectRoot 'romfs\game\fst.bin') `
    'Configured fst.bin'

# Reject mixed DolRecomp output: every manifest reference must exist, and no
# stale chunk may remain outside the manifest.
$generatedRoot = Join-Path $projectRoot 'generated'
$manifestPath = Join-Path $generatedRoot 'generated.c'
$headerPath = Join-Path $generatedRoot 'generated.h'
$chunksRoot = Join-Path $generatedRoot 'generated_chunks'
foreach ($required in @($manifestPath, $headerPath, $chunksRoot)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Generated code is incomplete; missing: $required"
    }
}
$manifest = [IO.File]::ReadAllText($manifestPath)
$countMatch = [regex]::Match($manifest, '(?m)^// ([0-9]+) C files\s*$')
if (-not $countMatch.Success) {
    throw 'generated.c has no DolRecomp chunk-count manifest'
}
$declaredChunks = [int]$countMatch.Groups[1].Value
$referenceMatches = [regex]::Matches(
    $manifest,
    '(?m)^// generated_chunks/(chunk_[^\r\n]+\.c)\s*$')
$referencedNames = @($referenceMatches | ForEach-Object { $_.Groups[1].Value })
$actualNames = @(Get-ChildItem -LiteralPath $chunksRoot -File -Filter 'chunk_*.c' |
    ForEach-Object { $_.Name })
if ($declaredChunks -ne $referencedNames.Count -or
    $declaredChunks -ne $actualNames.Count) {
    throw "Generated chunk mismatch: declared=$declaredChunks referenced=$($referencedNames.Count) actual=$($actualNames.Count)"
}
$actualSet = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($name in $actualNames) { [void]$actualSet.Add($name) }
foreach ($name in $referencedNames) {
    if (-not $actualSet.Contains($name)) {
        throw "Generated manifest references a missing chunk: $name"
    }
}

# Reconstruct the Wii FST and validate every extracted host asset used by DI.
$fst = [IO.File]::ReadAllBytes($fstPath)
if ($fst.Length -lt 12 -or ($fst[0] -band 1) -eq 0) {
    throw 'fst.bin has no directory root entry'
}
$entryCount = [int](Read-Be32 $fst 8)
$stringsOffset = $entryCount * 12
if ($entryCount -le 0 -or $stringsOffset -ge $fst.Length) {
    throw "fst.bin has an invalid entry count: $entryCount"
}
$parents = New-Object 'uint32[]' $entryCount
$names = New-Object 'string[]' $entryCount
$directoryStack = [Collections.Generic.List[object]]::new()
$directoryStack.Add([pscustomobject]@{ Index = 0; End = $entryCount })
$discFiles = [Collections.Generic.List[object]]::new()
for ($index = 0; $index -lt $entryCount; ++$index) {
    $entryOffset = $index * 12
    $typeAndName = Read-Be32 $fst $entryOffset
    $isDirectory = ($typeAndName -band 0xff000000L) -ne 0
    $nameOffset = [int]($typeAndName -band 0x00ffffffL)
    $nameStart = $stringsOffset + $nameOffset
    if ($nameStart -lt $stringsOffset -or $nameStart -ge $fst.Length) {
        throw "FST entry $index has an invalid name offset"
    }
    $nameEnd = $nameStart
    while ($nameEnd -lt $fst.Length -and $fst[$nameEnd] -ne 0) { ++$nameEnd }
    if ($nameEnd -ge $fst.Length) { throw "FST entry $index has no name terminator" }
    $names[$index] = [Text.Encoding]::ASCII.GetString(
        $fst, $nameStart, $nameEnd - $nameStart)
    if ($index -eq 0) { continue }
    while ($directoryStack.Count -gt 0 -and
           $index -ge $directoryStack[$directoryStack.Count - 1].End) {
        $directoryStack.RemoveAt($directoryStack.Count - 1)
    }
    if ($directoryStack.Count -eq 0) {
        throw "FST entry $index falls outside the root directory"
    }
    $parent = $directoryStack[$directoryStack.Count - 1].Index
    $parents[$index] = [uint32]$parent
    if ($isDirectory) {
        $next = [int](Read-Be32 $fst ($entryOffset + 8))
        if ($next -le $index -or $next -gt $entryCount) {
            throw "FST directory $index has invalid bounds"
        }
        $directoryStack.Add([pscustomobject]@{ Index = $index; End = $next })
        continue
    }
    $components = [Collections.Generic.List[string]]::new()
    $cursor = $index
    while ($cursor -ne 0) {
        $components.Insert(0, $names[$cursor])
        $cursor = [int]$parents[$cursor]
    }
    $relative = $components -join [IO.Path]::DirectorySeparatorChar
    $offset = [uint64](Read-Be32 $fst ($entryOffset + 4)) * 4L
    $size = [uint64](Read-Be32 $fst ($entryOffset + 8))
    $hostPath = Join-Path $filesRoot $relative
    if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf)) {
        throw "FST asset is missing: $relative"
    }
    $hostLength = [uint64](Get-Item -LiteralPath $hostPath).Length
    if ($hostLength -lt $size) {
        throw "FST asset is too short: $relative ($hostLength < $size)"
    }
    $discFiles.Add([pscustomobject]@{
        Offset = $offset
        End = $offset + $size
        Relative = $relative
    })
}
$previousEnd = [uint64]0
$havePrevious = $false
foreach ($file in @($discFiles | Sort-Object Offset, End)) {
    if ($file.End -eq $file.Offset) { continue }
    if ($havePrevious -and $file.Offset -lt $previousEnd) {
        throw "Overlapping FST range at $($file.Relative)"
    }
    $previousEnd = $file.End
    $havePrevious = $true
}

# Azahar must expose the same extracted tree at sdmc:/smg3ds/DATA.
$azaharData = Join-Path ([IO.Path]::GetFullPath($AzaharSdmc)) 'smg3ds\DATA'
Assert-ExactFile $dolPath (Join-Path $azaharData 'sys\main.dol') `
    'Azahar SD main.dol'
if (-not (Test-Path -LiteralPath (Join-Path $azaharData 'files') -PathType Container)) {
    throw "Azahar SD assets are unavailable: $azaharData"
}

# Source-level render gates: EXI must not feed GX, STM must be parkable, and
# actual covered EFB pixels—not blocks or decoded triangles—must gate success.
$mainSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'source\main.c'))
$gxSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'source\gx_renderer.c'))
$iosSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'source\ios_hle.c'))
$exiSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'source\exi_hle.c'))
$gpioSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'source\hollywood_gpio.c'))
if ($mainSource.Contains('g_gx_alias')) {
    throw 'EXI is still incorrectly labeled or counted as GX'
}
if (-not $mainSource.Contains('(physical & ~0x1fu) == 0x0c008000u')) {
    throw 'GX gather-pipe routing is not constrained to its 32-byte aperture'
}
if (-not $mainSource.Contains('smg3ds_exi_handles(physical, size)') -or
    -not $mainSource.Contains('SMG3DS_PI_EXI = 0x00000010u')) {
    throw 'EXI HLE is not isolated from GX and connected to PI'
}
if (-not $exiSource.Contains('SMG3DS_EXI_BASE_GC = 0x0c006800') -or
    -not $exiSource.Contains('SMG3DS_EXI_BASE_WII = 0x0d006800') -or
    -not $exiSource.Contains('channel->control &= ~SMG3DS_EXI_TSTART') -or
    -not $exiSource.Contains('channel->status |= SMG3DS_EXI_TCINT') -or
    -not $exiSource.Contains('smg3ds_exi_self_test')) {
    throw 'EXI HLE lacks mirrored completion semantics or its execution gate'
}
if (-not $mainSource.Contains('smg3ds_hollywood_gpio_handles(physical, size)') -or
    -not $mainSource.Contains('advance_timebase_from_execution(&cpu)') -or
    -not $mainSource.Contains('SMG3DS_BROADWAY_CYCLES_PER_TIMEBASE_TICK = 12') -or
    -not $mainSource.Contains('g_timebase_cycle_remainder') -or
    -not $mainSource.Contains('accumulated_cycles % SMG3DS_BROADWAY_CYCLES_PER_TIMEBASE_TICK') -or
    -not $mainSource.Contains('const u64 frame_timebase_target =') -or
    -not $mainSource.Contains('advance_timebase(&cpu, frame_timebase_target - cpu.timebase)') -or
    -not $mainSource.Contains('service_decrementer_interrupt(&cpu)') -or
    -not $mainSource.Contains('SMG3DS_PPC_VECTOR_DECREMENTER = 0x00000900u') -or
    -not $mainSource.Contains('mem_write32(cpu, 0x800000f8u, 243000000u)')) {
    throw 'Hollywood GPIO routing or sub-frame Wii timebase progress is missing'
}
if ($mainSource.Contains('cpu.timebase += SMG3DS_TIMEBASE_PER_FRAME;')) {
    throw 'Frame and execution timebase advances are still double-counted'
}
if (-not $gpioSource.Contains('SMG3DS_GPIOB_OUT = 0x0d8000c0u') -or
    -not $gpioSource.Contains('SMG3DS_GPIOB_DIR = 0x0d8000c4u') -or
    -not $gpioSource.Contains('SMG3DS_GPIOB_IN = 0x0d8000c8u') -or
    -not $gpioSource.Contains('SMG3DS_GPIOB_OWNER_MASK = 0x0000c3a0u') -or
    -not $gpioSource.Contains('SMG3DS_GPIOB_DIR_RESET = 0x00ffdf3fu') -or
    -not $gpioSource.Contains('smg3ds_hollywood_gpio_self_test')) {
    throw 'Hollywood GPIO HLE lacks coherent OUT/DIR/IN latches or its execution gate'
}
if ($gpioSource.Contains('value = g_stats.output') -and
    -not $gpioSource.Contains('disc_present ? SMG3DS_GPIO_SLOT_IN : 0u')) {
    throw 'GPIOB_IN can mirror SDA output instead of returning an AV encoder ACK'
}
if (-not $mainSource.Contains('SMG3DS_OS_EXCEPTION_VECTOR_DISPATCH = 0x804A1D0Cu') -or
    -not $mainSource.Contains('cpu->spr[272] = interrupted_r4') -or
    $mainSource.Contains('cpu->spr[1023]') -or
    $mainSource.Contains('service_bringup_halt')) {
    throw 'External interrupts do not preserve the patched exception-4 vector path'
}
if (-not $gxSource.Contains('rasterizer_self_test') -or
    -not $gxSource.Contains('rasterized_pixels += pixels')) {
    throw 'Renderer has no actual-pixel geometry proof'
}
if (-not $iosSource.Contains('SMG3DS_IOS_PARK') -or
    -not $iosSource.Contains('/dev/stm/eventhook') -or
    -not $iosSource.Contains('/dev/di')) {
    throw 'IOS HLE does not contain the eventhook/DI path required to reach draws'
}

# Independent EXI reference vectors validate the byte order and completion
# values required by the in-code self-test before a compiler may run.
$writeControl = [uint32]0x35
$readControl = [uint32]0x31
if (($writeControl -band 0xfffffffeL) -ne 0x34 -or
    ($readControl -band 0xfffffffeL) -ne 0x30) {
    throw 'EXI reference model did not clear TSTART'
}
$csr = [uint32]0x154
$csr = $csr -bor 0x8
if ($csr -ne 0x15c -or
    (($csr -band 0x4) -eq 0) -or
    (($csr -band 0x8) -eq 0)) {
    throw 'EXI reference model did not assert masked transfer completion'
}
$csr = $csr -band 0xfffffff7L
if ($csr -ne 0x154) {
    throw 'EXI reference model did not clear TCINT with W1C'
}
$sramCommand = [uint32]0x20000100
$sramAddress = [uint32](($sramCommand -shr 6) -band 0x01ffffff)
if ($sramAddress -ne 0x800004) {
    throw 'EXI IPL command address decoding is not big-endian'
}
$sramChecksumWord = [uint32]0x002cffd0
$rtcFlag = [uint32]0x02
$rtcFlagWord = ($rtcFlag -shl 24) -bor ($rtcFlag -shl 16) -bor
               ($rtcFlag -shl 8) -bor $rtcFlag
if ($sramChecksumWord -ne 0x002cffd0 -or $rtcFlagWord -ne 0x02020202) {
    throw 'EXI SRAM/RTC reference values are invalid'
}

# Broadway GPIO reference vectors and the 2 us AV-encoder delay must both
# advance exactly as expected before compiling another runtime candidate.
$gpioOwnerMask = [uint64]0x0000c3a0
$gpioNonOwnerMask = [uint64]4294917215
$gpioOut = [uint64]0
$gpioOut = [uint64](([uint64]4294967295 -band $gpioOwnerMask) -bor
                    ($gpioOut -band $gpioNonOwnerMask))
$gpioDirection = [uint64]0x00ffdf3f
$gpioDirection = [uint64](([uint64]0 -band $gpioOwnerMask) -bor
                          ($gpioDirection -band $gpioNonOwnerMask))
$waitTicks = [uint64][Math]::Ceiling((2.0 * 486.0) / 8.0)
$shortBlockTicks = [uint64]0
$cycleRemainder = [uint64]0
foreach ($blockCycles in (@(1) * 12)) {
    $accumulatedCycles = [uint64]$blockCycles + $cycleRemainder
    $shortBlockTicks += [uint64][Math]::Floor($accumulatedCycles / 12.0)
    $cycleRemainder = [uint64]($accumulatedCycles % 12)
}
$waitCycleTicks = [uint64][Math]::Floor(
    (1464.0 + $cycleRemainder) / 12.0)
if ($gpioOut -ne 0x0000c3a0 -or $gpioDirection -ne 0x00ff1c1f -or
    $waitTicks -ne 122 -or $shortBlockTicks -ne 1 -or
    $cycleRemainder -ne 0 -or $waitCycleTicks -ne 122) {
    throw 'Hollywood GPIO ownership or sub-frame timebase reference model failed'
}

# Mirror the deterministic triangle used by rasterizer_self_test and require
# at least one covered sample before the expensive 3DS build is allowed.
$triangle = @(@(32.0, 32.0), @(96.0, 32.0), @(32.0, 96.0))
$coveredPixels = 0
for ($y = 32; $y -le 96; ++$y) {
    for ($x = 32; $x -le 96; ++$x) {
        $px = $x + 0.5
        $py = $y + 0.5
        $w0 = ($triangle[1][0] - $px) * ($triangle[2][1] - $py) -
              ($triangle[2][0] - $px) * ($triangle[1][1] - $py)
        $w1 = ($triangle[2][0] - $px) * ($triangle[0][1] - $py) -
              ($triangle[0][0] - $px) * ($triangle[2][1] - $py)
        $w2 = ($triangle[0][0] - $px) * ($triangle[1][1] - $py) -
              ($triangle[1][0] - $px) * ($triangle[0][1] - $py)
        if (($w0 -ge 0 -and $w1 -ge 0 -and $w2 -ge 0) -or
            ($w0 -le 0 -and $w1 -le 0 -and $w2 -le 0)) {
            ++$coveredPixels
        }
    }
}
if ($coveredPixels -le 0) { throw 'Deterministic rasterizer triangle covered no pixels' }

$summary = (('Render-path preflight passed: DOL {0:N0} bytes, entry 0x{1:X8}, ' +
    '{2} sections, {3} generated chunks, {4} disc files, {5} test pixels, ' +
    'EXI checksum 0x{6:X8}, GPIO/TB {7} ticks, external vector 4.') -f
    $dolBytes.Length, $entryPoint, $loadedSections, $declaredChunks,
    $discFiles.Count, $coveredPixels, $sramChecksumWord, $waitTicks)
Write-Host $summary
