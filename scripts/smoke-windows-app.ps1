#Requires -Version 5.1
<#
.SYNOPSIS
  Exercises the installed Sync Windows app's lifecycle end to end.

.DESCRIPTION
  The Windows counterpart to scripts/smoke-macos-app.sh. It launches the tray
  app, waits for the managed helper to answer /status, and then asserts the
  guarantee that actually matters for a supervised helper: quitting the app
  must take the helper with it.

  Provider AVAILABILITY is deliberately not asserted by default. A CI runner
  has no GPU and no OpenGL context, so Spout cannot initialise there, and the
  NDI runtime is a user-installed dependency that CI does not have. Asserting
  availability would make this script pass only on a workstation. Pass
  -RequireProvider <id> on a real machine to assert it.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$Bundle,

  [string]$RequireProvider = '',

  [int]$Port = 53979
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$message) {
  Write-Error "smoke-windows-app: $message"
  exit 1
}

if (-not [System.IO.Path]::IsPathRooted($Bundle) -or
    -not (Test-Path -LiteralPath $Bundle -PathType Container)) {
  Fail "an absolute bundle directory is required, got '$Bundle'"
}
$Bundle = (Resolve-Path -LiteralPath $Bundle).ProviderPath
$trayExe = Join-Path $Bundle 'Sync.exe'
if (-not (Test-Path -LiteralPath $trayExe -PathType Leaf)) {
  Fail "missing $trayExe"
}

$occupied = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if ($null -ne $occupied) {
  Fail "TCP $Port is already occupied"
}

$tray = $null
$statusUri = "http://127.0.0.1:$Port/status"

# The preview notice is a per-user registry value, so like the macOS script
# borrows the defaults key, this borrows the value and puts it back on exit.
$noticeKey = 'HKCU:\Software\Noise Factor\Sync'
$noticeName = 'PreviewNoticeShown'
$noticeExisted = $false
$noticePrevious = $null
if (Test-Path -LiteralPath $noticeKey) {
  $existing = Get-ItemProperty -LiteralPath $noticeKey -Name $noticeName -ErrorAction SilentlyContinue
  if ($null -ne $existing) {
    $noticeExisted = $true
    $noticePrevious = $existing.$noticeName
  }
} else {
  New-Item -Path $noticeKey -Force | Out-Null
}
Set-ItemProperty -LiteralPath $noticeKey -Name $noticeName -Value 1 -Type DWord

try {
  $tray = Start-Process -FilePath $trayExe -PassThru

  $health = $null
  for ($attempt = 0; $attempt -lt 100; $attempt++) {
    if ($tray.HasExited) {
      Fail "Sync exited before becoming healthy (exit code $($tray.ExitCode))"
    }
    try {
      $health = Invoke-RestMethod -Uri $statusUri -TimeoutSec 1 -ErrorAction Stop
      break
    } catch {
      Start-Sleep -Milliseconds 100
    }
  }
  if ($null -eq $health) { Fail 'Sync did not become healthy' }

  if ($health.product -ne 'Sync') { Fail "unexpected product '$($health.product)'" }
  if ($health.status -ne 'ok') { Fail "unexpected status '$($health.status)'" }

  $providerIds = @($health.capabilities.providers | ForEach-Object { $_.id })
  foreach ($expected in @('spout', 'ndi')) {
    if ($providerIds -notcontains $expected) {
      Fail "the Windows build must advertise the '$expected' provider, saw: $($providerIds -join ', ')"
    }
  }
  if (-not [string]::IsNullOrWhiteSpace($RequireProvider)) {
    $required = $health.capabilities.providers |
      Where-Object { $_.id -eq $RequireProvider -and $_.available -and $_.selected }
    if ($null -eq $required) {
      Fail "provider '$RequireProvider' is not available and selected"
    }
  }

  $helpers = @(Get-CimInstance Win32_Process -Filter "Name = 'syncd.exe'" |
    Where-Object { $_.ParentProcessId -eq $tray.Id })
  if ($helpers.Count -eq 0) { Fail 'managed helper was not found' }
  $helperIds = @($helpers | ForEach-Object { $_.ProcessId })

  # The job object is what guarantees the helper cannot outlive the tray app.
  # Closing the tray app is therefore the assertion, not a cleanup step.
  $tray.CloseMainWindow() | Out-Null
  if (-not $tray.WaitForExit(5000)) {
    $tray.Kill()
    Fail 'app did not quit within five seconds'
  }

  for ($attempt = 0; $attempt -lt 50; $attempt++) {
    $survivors = @(Get-Process -Id $helperIds -ErrorAction SilentlyContinue)
    if ($survivors.Count -eq 0) { break }
    Start-Sleep -Milliseconds 100
  }
  $survivors = @(Get-Process -Id $helperIds -ErrorAction SilentlyContinue)
  if ($survivors.Count -ne 0) { Fail 'helper survived app quit' }

  $tray = $null
  Write-Output 'Sync app lifecycle smoke passed'
} finally {
  if ($null -ne $tray -and -not $tray.HasExited) {
    Stop-Process -Id $tray.Id -Force -ErrorAction SilentlyContinue
  }
  Get-Process -Name 'syncd' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
  if ($noticeExisted) {
    Set-ItemProperty -LiteralPath $noticeKey -Name $noticeName -Value $noticePrevious -Type DWord
  } else {
    Remove-ItemProperty -LiteralPath $noticeKey -Name $noticeName -ErrorAction SilentlyContinue
  }
}
