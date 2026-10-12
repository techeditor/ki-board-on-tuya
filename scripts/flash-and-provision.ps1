<#
.SYNOPSIS
  One command: flash the generic firmware bin, then provision a per-device Tuya
  license (UUID/AuthKey) over USB from a local licenses.json.

.DESCRIPTION
  Chains the two existing steps so a whole board is brought up with one call:

    1. flash.ps1   - pull the latest GENERIC QIO bin from the EC2 devbox and
                     flash it to the COM port with tyutool (unless -SkipFlash).
    2. (wait)      - let tyutool fully release the COM port and the device boot.
    3. provision-license.ps1 - open the SAME COM port, handshake, and inject the
                     chosen license entry; verify the write.

  The firmware is a generic image carrying NO real device secret; the license is
  injected at this step, so one bin serves every board and each device gets its
  own identity from licenses.json. See provision-license.ps1 for the protocol.

  Flashing and provisioning share ONE COM port and are strictly serial: step 3
  does not open the port until tyutool has exited and released it.

.PARAMETER Port
  Serial COM port (e.g. COM12). Required.

.PARAMETER Alias
  Which licenses.json entry to inject (optional when the file has one entry).

.PARAMETER App
  Remote app dir to flash. Default: firmware/app (the product firmware).

.PARAMETER KeyFile
  licenses.json path. Default: scripts\licenses.json

.PARAMETER SkipFlash
  Skip step 1/2 and only provision a board already running the generic bin.

.PARAMETER Build
  Pass through to flash.ps1 to rebuild on the devbox before flashing.

.PARAMETER Monitor
  Open a serial monitor AFTER provisioning (to watch the device go online).

.PARAMETER SettleSec
  Seconds to wait after flashing for port release + device boot. Default 6.

.PARAMETER SshHost
  SSH alias for the EC2 devbox (passed to flash.ps1). Default: devbox-ssm

.PARAMETER ForceProvision
  Always (re)write the license even if the device already holds the target UUID.
  Passed through to provision-license.ps1. Use when rotating a key.

.PARAMETER NoSkipIfMatch
  Disable the idempotent skip (provision always writes). By default a re-flash
  with an unchanged key only reads back and confirms, performing no write.

.EXAMPLE
  .\flash-and-provision.ps1 -Port COM12 -Alias Orange

.EXAMPLE
  # re-flash, key unchanged -> provisioning auto-skips the write
  .\flash-and-provision.ps1 -Port COM12 -Alias Orange

.EXAMPLE
  # force a license re-write after flashing
  .\flash-and-provision.ps1 -Port COM12 -Alias Orange -ForceProvision

.EXAMPLE
  .\flash-and-provision.ps1 -Port COM12 -Alias Orange -SkipFlash
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$Alias,
    [string]$App = "firmware/app",
    [string]$KeyFile,
    [switch]$SkipFlash,
    [switch]$Build,
    [switch]$Monitor,
    [int]$SettleSec = 6,
    [string]$SshHost = "devbox-ssm",
    [switch]$ForceProvision,
    [switch]$NoSkipIfMatch
)

$ErrorActionPreference = "Stop"
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$flashScript = Join-Path $scriptDir "flash.ps1"
$provScript  = Join-Path $scriptDir "provision-license.ps1"

function Fail($msg) { Write-Host "[ERROR] $msg" -ForegroundColor Red; exit 1 }
function Info($msg) { Write-Host "[STEP] $msg" -ForegroundColor Cyan }

if (-not (Test-Path $provScript)) { Fail "provision-license.ps1 not found next to this script." }

# ---- step 1+2: flash the generic bin, then let the port settle -----------
if (-not $SkipFlash) {
    if (-not (Test-Path $flashScript)) { Fail "flash.ps1 not found next to this script." }
    Info "Flashing generic firmware ($App) to $Port ..."
    $flashParams = @{ Port = $Port; App = $App; SshHost = $SshHost }
    if ($Build) { $flashParams.Build = $true }
    & $flashScript @flashParams
    if ($LASTEXITCODE -ne 0) { Fail "flash.ps1 failed (exit $LASTEXITCODE); not provisioning." }

    Info "Waiting ${SettleSec}s for tyutool to release $Port and the device to boot ..."
    Start-Sleep -Seconds $SettleSec
} else {
    Info "SkipFlash: provisioning a board already running the generic bin."
}

# ---- step 3: provision the license over the same port --------------------
Info "Provisioning license on $Port ..."
$provParams = @{ Port = $Port }
if ($Alias)          { $provParams.Alias   = $Alias }
if ($KeyFile)        { $provParams.KeyFile = $KeyFile }
if ($ForceProvision) { $provParams.ForceProvision = $true }
if ($NoSkipIfMatch)  { $provParams.SkipIfMatch = $false }
& $provScript @provParams
if ($LASTEXITCODE -ne 0) { Fail "provision-license.ps1 failed (exit $LASTEXITCODE)." }

Write-Host "[DONE] Flashed + provisioned $Port" -ForegroundColor Green

# ---- optional monitor ----------------------------------------------------
if ($Monitor) {
    Info "Opening serial monitor on $Port ..."
    & $flashScript -Port $Port -App $App -NoPull -Monitor -SshHost $SshHost
}
