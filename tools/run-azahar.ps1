[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Azahar,
    [string]$Application
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$azaharPath = (Resolve-Path -LiteralPath $Azahar).Path
if (-not $Application) { $Application = Join-Path $root 'smg3ds-bringup.3dsx' }
$applicationPath = (Resolve-Path -LiteralPath $Application).Path
$quotedApplication = '"' + $applicationPath + '"'
Start-Process -FilePath $azaharPath -ArgumentList $quotedApplication
