<#
.SYNOPSIS
  Provision a Tuya device license (UUID/AuthKey) onto an already-flashed board
  over the USB serial link, reading the values from a local licenses.json.

.DESCRIPTION
  New provisioning model: firmware is a GENERIC image (built on EC2) that carries
  no real device secret. After flashing, this script injects the per-device
  license over USB/UART0 using the firmware's existing JSONL protocol:

    1. open the COM port at 115200 8N1
    2. send {"type":"hello"} and wait for hello_ack (locks the USB transport)
    3. send {"v":1,"seq":N,"type":"license","payload":{"uuid":"..","authkey":".."}}
    4. wait for license_status:written
    5. send {"type":"license_read"} and confirm the returned uuid matches

  The license write is accepted by the firmware ONLY over the physically trusted
  USB/UART transport (BLE is rejected), so the USB flashing cable satisfies it.
  PID stays compile-time in the generic image; only UUID/AuthKey are injected.

  The AuthKey value is never printed; only its length and the UUID are shown.

.PARAMETER KeyFile
  Path to licenses.json (see licenses.json.example). Default: scripts\licenses.json

.PARAMETER Alias
  Which license entry to use (matches the "alias" field, case-insensitive).
  Optional when the file holds exactly one entry.

.PARAMETER Port
  Serial COM port the board is on (e.g. COM12). Required.

.PARAMETER Baud
  Application UART0 baud. Default 115200.

.PARAMETER TimeoutSec
  Per-step reply timeout in seconds. Default 10.

.PARAMETER ExpectPid
  Optional PID the firmware was built with. When set, the chosen entry's
  product_id must match it, guarding against sending a license to the wrong
  build. Omit to skip the check (the entry's product_id is still displayed).

.PARAMETER SkipIfMatch
  Idempotent re-flash support (ON by default). Before writing, read the license
  already on the device (license_read); if its UUID equals the entry's UUID, the
  write is SKIPPED and the device is reported already-provisioned. This makes a
  repeat run with an unchanged key behave like "flash only" (one quick read, no
  write). A mismatch or an empty/unreadable license still triggers a write.
  Pass -SkipIfMatch:$false to always write.

.PARAMETER ForceProvision
  Always write the license regardless of what the device already holds. Overrides
  -SkipIfMatch. Use to re-write a key or recover a board with a wrong/partial
  license.

.EXAMPLE
  .\provision-license.ps1 -Port COM12 -Alias Orange

.EXAMPLE
  # key unchanged -> read-only confirm, no write
  .\provision-license.ps1 -Port COM12 -Alias Orange

.EXAMPLE
  # force a re-write even if the device already matches
  .\provision-license.ps1 -Port COM12 -Alias Orange -ForceProvision

.EXAMPLE
  .\provision-license.ps1 -Port COM12 -KeyFile C:\keys\licenses.json -Alias Purple
#>
[CmdletBinding()]
param(
    [string]$KeyFile,
    [string]$Alias,
    [Parameter(Mandatory = $true)][string]$Port,
    [int]$Baud = 115200,
    [int]$TimeoutSec = 10,
    [string]$ExpectPid,
    [switch]$SkipIfMatch = $true,
    [switch]$ForceProvision
)

$ErrorActionPreference = "Stop"

function Fail($msg) { Write-Host "[ERROR] $msg" -ForegroundColor Red; exit 1 }
function Info($msg) { Write-Host "[INFO] $msg" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "[ OK ] $msg" -ForegroundColor Green }

# ---- resolve + load the key file -----------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $KeyFile) { $KeyFile = Join-Path $scriptDir "licenses.json" }
if (-not (Test-Path $KeyFile)) {
    Fail "Key file not found: $KeyFile  (copy licenses.json.example to licenses.json and fill it)"
}

try {
    $doc = Get-Content -Raw -Path $KeyFile | ConvertFrom-Json
} catch {
    Fail "Could not parse JSON in $KeyFile : $($_.Exception.Message)"
}
if (-not $doc.licenses -or $doc.licenses.Count -lt 1) {
    Fail "No 'licenses' array (or it is empty) in $KeyFile"
}

# ---- pick the entry -------------------------------------------------------
if ($Alias) {
    $entry = $doc.licenses | Where-Object { $_.alias -and ($_.alias.ToLowerInvariant() -eq $Alias.ToLowerInvariant()) } | Select-Object -First 1
    if (-not $entry) { Fail "No license entry with alias '$Alias' in $KeyFile" }
} elseif ($doc.licenses.Count -eq 1) {
    $entry = $doc.licenses[0]
} else {
    $names = ($doc.licenses | ForEach-Object { $_.alias }) -join ", "
    Fail "Key file has multiple entries ($names); specify -Alias"
}

$uuid    = "$($entry.uuid)".Trim()
$authkey = "$($entry.authkey)".Trim()
$pid_    = "$($entry.product_id)".Trim()
$aliasNm = "$($entry.alias)".Trim()

# ---- validate -------------------------------------------------------------
if ($uuid.Length -ne 20) { Fail "uuid for '$aliasNm' must be 20 chars, got $($uuid.Length)" }
if ($authkey.Length -ne 32) { Fail "authkey for '$aliasNm' must be 32 chars, got $($authkey.Length)" }
if ($uuid -notmatch '^[A-Za-z0-9]+$') { Fail "uuid for '$aliasNm' has non-alphanumeric chars" }
if ($authkey -notmatch '^[A-Za-z0-9]+$') { Fail "authkey for '$aliasNm' has non-alphanumeric chars" }
if ($ExpectPid -and $pid_ -and ($pid_ -ne $ExpectPid)) {
    Fail "product_id mismatch: entry '$aliasNm' is '$pid_' but firmware expects '$ExpectPid'"
}

Info "License entry: alias=$aliasNm  pid=$pid_  uuid=$uuid  authkey=<32 chars hidden>"
Info "Target port:   $Port @ $Baud 8N1"

# ---- serial helpers -------------------------------------------------------
$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.ReadTimeout  = 500
$sp.WriteTimeout = 2000
$sp.NewLine      = "`n"

# Read JSONL lines until a line whose "type" matches one of $wantTypes, or timeout.
function Wait-Frame([string[]]$wantTypes, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    $buf = ""
    while ((Get-Date) -lt $deadline) {
        try { $buf += $sp.ReadExisting() } catch {}
        while ($buf -match "`n") {
            $idx  = $buf.IndexOf("`n")
            $line = $buf.Substring(0, $idx).Trim("`r"," ")
            $buf  = $buf.Substring($idx + 1)
            if ($line.Length -eq 0) { continue }
            if ($line[0] -ne '{') { continue }   # skip human log lines
            $obj = $null
            try { $obj = $line | ConvertFrom-Json } catch { continue }
            if ($obj.type -and ($wantTypes -contains $obj.type)) { return $obj }
        }
        Start-Sleep -Milliseconds 50
    }
    return $null
}

function Send-Line([string]$json) {
    $sp.Write($json + "`n")
}

# ---- run the handshake + provisioning ------------------------------------
try {
    try { $sp.Open() } catch { Fail "Could not open ${Port}: $($_.Exception.Message)  (is the flasher/monitor still holding it?)" }
    Start-Sleep -Milliseconds 200
    try { $sp.DiscardInBuffer(); $sp.DiscardOutBuffer() } catch {}

    Info "Handshaking (hello -> hello_ack)..."
    $ack = $null
    for ($i = 1; $i -le 5 -and -not $ack; $i++) {
        Send-Line '{"v":1,"type":"hello"}'
        $ack = Wait-Frame @("hello_ack") 3
        if (-not $ack) { Info "  no hello_ack yet (try $i/5); device may still be booting..." }
    }
    if (-not $ack) { Fail "No hello_ack from device. Check the port, that the board is powered, and that it runs the product (DUAL/UART) firmware." }
    Ok "hello_ack received (fw=$($ack.payload.fw), transport=$($ack.payload.transport))"

    # ---- idempotent pre-check: skip the write if the device already matches ----
    if ($ForceProvision) {
        Info "ForceProvision: writing license regardless of what the device holds."
    } elseif ($SkipIfMatch) {
        Info "Checking existing license on device (license_read)..."
        $sp.DiscardInBuffer()
        Send-Line '{"v":1,"type":"license_read"}'
        $pre = Wait-Frame @("license_info") $TimeoutSec
        if ($pre) {
            $preUuid = "$($pre.payload.uuid)".Trim()
            $preSrc  = "$($pre.payload.source)"
            if ($preUuid -and ($preUuid -eq $uuid)) {
                Ok "Device already holds this license (source=$preSrc uuid=$preUuid) -- skipping write."
                Ok "No change needed for '$aliasNm' on $Port"
                $sp.Close(); $sp.Dispose()
                exit 0
            } elseif ($preUuid) {
                Info "Device UUID '$preUuid' differs from target '$uuid' -- will write."
            } else {
                Info "Device has no stored license yet -- will write."
            }
        } else {
            Info "No license_read reply (device may have no license) -- will write."
        }
    }

    $seq = Get-Random -Minimum 1 -Maximum 65000
    Info "Sending license frame (seq=$seq)..."
    $payload = @{ v = 1; seq = $seq; type = "license"; payload = @{ uuid = $uuid; authkey = $authkey } } | ConvertTo-Json -Compress
    Send-Line $payload

    $status = Wait-Frame @("license_status") $TimeoutSec
    if (-not $status) { Fail "No license_status reply within $TimeoutSec s." }
    $state = "$($status.payload.state)"
    if ($state -ne "written") { Fail "Device reported license state '$state' (expected 'written')." }
    Ok "Device accepted the license (state=written)"

    Info "Verifying (license_read)..."
    Send-Line '{"v":1,"type":"license_read"}'
    $info = Wait-Frame @("license_info") $TimeoutSec
    if (-not $info) {
        Write-Host "[WARN] No license_info echo (write reported OK but read-back unconfirmed)." -ForegroundColor Yellow
    } else {
        $src = "$($info.payload.source)"; $ruuid = "$($info.payload.uuid)"
        if ($ruuid -and ($ruuid -ne $uuid)) {
            Fail "Read-back UUID '$ruuid' does not match sent '$uuid'."
        }
        Ok "Verified: source=$src uuid=$ruuid"
    }

    Ok "Provisioned '$aliasNm' license onto device on $Port"
} finally {
    if ($sp -and $sp.IsOpen) { $sp.Close() }
    if ($sp) { $sp.Dispose() }
}
