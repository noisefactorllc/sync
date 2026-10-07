<#
.SYNOPSIS
  Real WASAPI driver qualification using virtual audio loopback in Windows CI.
.DESCRIPTION
  Starts necessary Windows audio services, verifies application microphone
  permissions, enumerates real WASAPI input endpoints via sync_audio_native_probe,
  and executes a live 2-second capture qualification verifying format negotiation,
  packet continuity, and zero dropped frames. Every capture attempt's report is
  retained under audio-qualification/wasapi/capture-attempts/, and only startup
  unavailability (an endpoint that produced no frames) is retried: a substantive
  integrity failure fails the qualification immediately, because a later clean
  capture cannot qualify a source whose samples were bad.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)]
  [string]$BuildDir,
  [Parameter(Mandatory=$true)]
  [string]$ArtifactDir
)

$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Path $ArtifactDir -Force | Out-Null
$probe = Join-Path $BuildDir "sync_audio_native_probe.exe"
if (-not (Test-Path $probe)) {
  throw "sync_audio_native_probe.exe not found in $BuildDir"
}

# The service refresh and the AppPrivacy policy exist only on Windows. The
# enumeration and qualification logic below is cross-platform: it runs against
# the audio fixture backend and scripted probe responses elsewhere.
$isWindowsOs = ($PSVersionTable.Platform -eq 'Win32NT') -or ($env:OS -eq 'Windows_NT')

# 1. Ensure Windows audio services are running and refreshed to detect newly installed hardware.
if ($isWindowsOs) {
  Write-Output "Refreshing Windows Audio services to discover endpoints..."
  Restart-Service AudioEndpointBuilder -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
  Start-Service Audiosrv -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 1

  # 2. Allow microphone access via AppPrivacy policy.
  $privacyPath = "HKLM:\SOFTWARE\Policies\Microsoft\Windows\AppPrivacy"
  if (-not (Test-Path $privacyPath)) {
    New-Item -Path $privacyPath -Force | Out-Null
  }
  New-ItemProperty -Path $privacyPath -Name "LetAppsAccessMicrophone" -Value 1 -PropertyType DWord -Force | Out-Null
}

# 3. Inventory WASAPI audio devices with retry for endpoint initialization.
$inventoryPath = Join-Path $ArtifactDir "inventory.json"
$inventory = $null
for ($attempt = 1; $attempt -le 10; $attempt++) {
  $rawList = & $probe --list
  if ($LASTEXITCODE -eq 0 -and $rawList) {
    try {
      $parsed = $rawList | ConvertFrom-Json
      if ($parsed.sources -and $parsed.sources.Count -gt 0) {
        $inventory = $parsed
        $rawList | Out-File -FilePath $inventoryPath -Encoding utf8
        Write-Output "Enumerated $($inventory.sources.Count) WASAPI audio source(s):"
        foreach ($src in $inventory.sources) {
          Write-Output "  - $($src.name) ($($src.id), $($src.channelCount) ch @ $($src.sampleRate) Hz)"
        }
        break
      }
    } catch {
      Write-Warning "Attempt ${attempt}: failed to parse inventory output: $_"
    }
  }
  Write-Output "Waiting for WASAPI audio endpoints to register (attempt $attempt/10)..."
  Start-Sleep -Seconds 2
}

if (-not $inventory -or -not $inventory.sources -or $inventory.sources.Count -eq 0) {
  throw "WASAPI audio inventory returned 0 sources; virtual audio loopback driver is required"
}

# 4. Qualify real capture from the first enumerated source. Only startup
#    unavailability is retried (endpoint warmup); a substantive integrity
#    failure is not, and every attempt's report is retained.
$source = $inventory.sources[0]
Write-Output "Qualifying WASAPI capture from source: $($source.name) ($($source.id))"

$captureDir = Join-Path $ArtifactDir "capture-attempts"
New-Item -ItemType Directory -Path $captureDir -Force | Out-Null
$qualified = $false
$probeExitCode = 1
for ($attempt = 1; $attempt -le 5; $attempt++) {
  $captureOutput = & $probe --source-id $source.id
  $probeExitCode = $LASTEXITCODE
  $attemptPath = Join-Path $captureDir ("attempt-{0}.json" -f $attempt)
  if ($captureOutput) {
    $captureOutput | Out-File -FilePath $attemptPath -Encoding utf8
  }
  $attemptClass = $null
  if (Test-Path $attemptPath) {
    try {
      $attemptReport = Get-Content $attemptPath -Raw | ConvertFrom-Json
      $attemptClass = $attemptReport.failureClass
    } catch {
      Write-Warning "Attempt ${attempt}: failed to parse capture output: $_"
    }
  }
  if ($attemptClass -eq 'integrity') {
    # A real sample integrity failure cannot become qualification through a
    # later retry, so the qualification stops here with the report retained.
    Write-Output "PROBE: $captureOutput"
    throw "WASAPI capture integrity failure on attempt ${attempt} (exit code $probeExitCode); not retryable"
  }
  if ($probeExitCode -eq 0 -and (Test-Path $attemptPath)) {
    try {
      $capture = Get-Content $attemptPath -Raw | ConvertFrom-Json
      if ($capture.qualified) {
        $qualified = $true
        break
      }
    } catch {
      Write-Warning "Attempt ${attempt}: failed to parse capture output: $_"
    }
  }
  if ($attempt -lt 5) {
    # An absent or unparseable report ($attemptClass is null) is retried
    # conservatively, but the retained output says so instead of labeling
    # missing evidence as startup.
    $attemptClassLabel = if ($attemptClass) { $attemptClass } else { 'unclassified' }
    Write-Output "WASAPI capture qualification attempt $attempt/5 did not qualify ($attemptClassLabel, exit code $probeExitCode); retrying in 2s..."
    Start-Sleep -Seconds 2
  }
}

if (-not $qualified) {
  Write-Output "sync_audio_native_probe failed to qualify (exit code $probeExitCode)"
  Get-ChildItem -Path $captureDir -Filter 'attempt-*.json' | Sort-Object Name | ForEach-Object {
    Write-Output "PROBE ($($_.Name)): $(Get-Content -Raw $_.FullName)"
  }
  throw "WASAPI audio capture qualification failed with exit code $probeExitCode"
}
Write-Output "WASAPI audio source $($source.id) successfully qualified"
