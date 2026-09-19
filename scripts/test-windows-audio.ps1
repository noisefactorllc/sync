<#
.SYNOPSIS
  Real WASAPI driver qualification using virtual audio loopback in Windows CI.
.DESCRIPTION
  Starts necessary Windows audio services, verifies application microphone
  permissions, enumerates real WASAPI input endpoints via sync_audio_native_probe,
  and executes a live 2-second capture qualification verifying format negotiation,
  packet continuity, and zero dropped frames.
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

# 1. Ensure Windows audio services are running.
$services = @('Audiosrv', 'AudioEndpointBuilder')
foreach ($name in $services) {
  $svc = Get-Service -Name $name -ErrorAction SilentlyContinue
  if ($svc) {
    if ($svc.Status -ne 'Running') {
      Set-Service -Name $name -StartupType Automatic
      Start-Service -Name $name
    }
  }
}

# 2. Allow microphone access via AppPrivacy policy.
$privacyPath = "HKLM:\SOFTWARE\Policies\Microsoft\Windows\AppPrivacy"
if (-not (Test-Path $privacyPath)) {
  New-Item -Path $privacyPath -Force | Out-Null
}
New-ItemProperty -Path $privacyPath -Name "LetAppsAccessMicrophone" -Value 1 -PropertyType DWord -Force | Out-Null

# 3. Inventory WASAPI audio devices.
$inventoryPath = Join-Path $ArtifactDir "inventory.json"
& $probe --list | Tee-Object -FilePath $inventoryPath
if ($LASTEXITCODE -ne 0) {
  throw "WASAPI audio device inventory failed with exit code $LASTEXITCODE"
}

$inventory = Get-Content $inventoryPath -Raw | ConvertFrom-Json
if ($inventory.sources.Count -eq 0) {
  throw "WASAPI audio inventory returned 0 sources; virtual audio loopback driver is required"
}

# 4. Qualify real capture from the first enumerated source.
$source = $inventory.sources[0]
Write-Output "Qualifying WASAPI capture from source: $($source.name) ($($source.id))"

$capturePath = Join-Path $ArtifactDir "capture.json"
& $probe --source-id $source.id | Tee-Object -FilePath $capturePath
if ($LASTEXITCODE -ne 0) {
  throw "WASAPI audio capture qualification failed with exit code $LASTEXITCODE"
}

$capture = Get-Content $capturePath -Raw | ConvertFrom-Json
if (-not $capture.qualified) {
  throw "WASAPI audio source $($source.id) failed qualification: $($capture | ConvertTo-Json -Compress)"
}
Write-Output "WASAPI audio source $($source.id) successfully qualified"
