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

# The tray app owns a real top-level window that it never shows -- it exists
# only to receive the tray icon callback and timer messages. Windows reports
# MainWindowHandle as 0 for a window that was never shown, so
# Process.CloseMainWindow() silently does nothing and the app never quits.
# Finding it by its registered class and posting WM_CLOSE is what actually
# drives the app's own shutdown path, which is the thing under test.
Add-Type -Namespace SyncSmoke -Name Win32 -MemberDefinition @'
public delegate bool EnumProc(IntPtr window, IntPtr context);
[DllImport("user32.dll")]
[return: MarshalAs(UnmanagedType.Bool)]
public static extern bool EnumWindows(EnumProc callback, IntPtr context);
[DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern int GetClassNameW(IntPtr window, System.Text.StringBuilder name, int size);
[DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
[return: MarshalAs(UnmanagedType.Bool)]
public static extern bool PostMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);
'@

# Every top-level window owned by one process, as (handle, class) pairs.
function Get-ProcessWindow([int]$processId) {
  $windows = New-Object System.Collections.ArrayList
  $callback = [SyncSmoke.Win32+EnumProc]{
    param($window, $context)
    # Required by the EnumWindows delegate signature; nothing here needs it.
    $null = $context
    $owner = 0
    [void][SyncSmoke.Win32]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($owner -eq $processId) {
      $name = New-Object System.Text.StringBuilder 256
      [void][SyncSmoke.Win32]::GetClassNameW($window, $name, 256)
      [void]$windows.Add([pscustomobject]@{ Handle = $window; Class = $name.ToString() })
    }
    return $true
  }
  [void][SyncSmoke.Win32]::EnumWindows($callback, [IntPtr]::Zero)
  return $windows
}

# Must match window_class.lpszClassName in app_main.cpp.
$trayWindowClass = 'NoiseFactorSyncTrayWindow'
$WM_CLOSE = 0x0010

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

$trayOut = Join-Path ([IO.Path]::GetTempPath()) "sync-smoke-tray-out.txt"
$trayErr = Join-Path ([IO.Path]::GetTempPath()) "sync-smoke-tray-err.txt"

# Everything this knows about why a run failed, printed on the way out.
# Without it a failure is just "did not become healthy", which is not
# something anyone can act on from a CI log.
function Show-FailureContext([string]$context) {
  Write-Host "---- smoke diagnostics: $context ----"
  if ($null -ne $tray) {
    Write-Host "tray pid=$($tray.Id) hasExited=$($tray.HasExited)"
    if ($tray.HasExited) { Write-Host "tray exit code=$($tray.ExitCode)" }
  } else {
    Write-Host "tray was never started"
  }
  foreach ($pair in @(@("stdout", $trayOut), @("stderr", $trayErr))) {
    if (Test-Path -LiteralPath $pair[1]) {
      $text = (Get-Content -LiteralPath $pair[1] -Raw -ErrorAction SilentlyContinue)
      if ([string]::IsNullOrWhiteSpace($text)) { $text = "(empty)" }
      Write-Host "tray $($pair[0]): $text"
    }
  }
  if ($null -ne $tray -and -not $tray.HasExited) {
    $windows = @(Get-ProcessWindow $tray.Id)
    Write-Host "tray windows: $(($windows | ForEach-Object { $_.Class }) -join ', ')"
    # #32770 is the standard dialog class. A modal dialog blocks the message
    # loop, which would explain a WM_CLOSE that is never acted on -- most
    # likely the first-run preview notice failing to be suppressed.
    if ($windows | Where-Object { $_.Class -eq '#32770' }) {
      Write-Host "a modal dialog is open; the first-run notice was probably not suppressed"
    }
  }
  $running = @(Get-Process -Name syncd -ErrorAction SilentlyContinue)
  Write-Host "syncd processes running: $($running.Count)"
  $listening = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
  Write-Host "listening on $Port : $(if ($null -eq $listening) { 'no' } else { 'yes' })"

  # Launch the helper on its own. This separates "the helper cannot run
  # here" from "the tray app failed to start or supervise it", which are
  # very different problems and indistinguishable from the tray app alone.
  $helperExe = Join-Path $Bundle 'syncd.exe'
  if (Test-Path -LiteralPath $helperExe) {
    $ho = Join-Path ([IO.Path]::GetTempPath()) "sync-smoke-helper-out.txt"
    $he = Join-Path ([IO.Path]::GetTempPath()) "sync-smoke-helper-err.txt"
    $helper = Start-Process -FilePath $helperExe -ArgumentList "--port","54999" `
      -PassThru -RedirectStandardOutput $ho -RedirectStandardError $he
    Start-Sleep -Seconds 3
    Write-Host "direct helper hasExited=$($helper.HasExited)"
    if ($helper.HasExited) { Write-Host "direct helper exit code=$($helper.ExitCode)" }
    foreach ($pair in @(@("stdout", $ho), @("stderr", $he))) {
      if (Test-Path -LiteralPath $pair[1]) {
        $text = (Get-Content -LiteralPath $pair[1] -Raw -ErrorAction SilentlyContinue)
        if ([string]::IsNullOrWhiteSpace($text)) { $text = "(empty)" }
        Write-Host "direct helper $($pair[0]): $text"
      }
    }
    if (-not $helper.HasExited) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
  }
  Write-Host "---- end diagnostics ----"
}

try {
  $tray = Start-Process -FilePath $trayExe -PassThru `
    -RedirectStandardOutput $trayOut -RedirectStandardError $trayErr

  $health = $null
  for ($attempt = 0; $attempt -lt 100; $attempt++) {
    if ($tray.HasExited) {
      Show-FailureContext 'tray app exited early'
      Fail "Sync exited before becoming healthy (exit code $($tray.ExitCode))"
    }
    try {
      $health = Invoke-RestMethod -Uri $statusUri -TimeoutSec 1 -ErrorAction Stop
      break
    } catch {
      Start-Sleep -Milliseconds 100
    }
  }
  if ($null -eq $health) {
    Show-FailureContext 'health probe never succeeded'
    Fail 'Sync did not become healthy'
  }

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
  # Hold live Process objects, not just ids: once a process exits, its exit
  # code is only readable through a handle acquired while it was running.
  # That code is what separates "asked to stop and did" from "was killed".
  $helperProcesses = @(Get-Process -Id $helperIds -ErrorAction SilentlyContinue)
  foreach ($helper in $helperProcesses) {
    # Touching .Handle now is what actually opens and caches it. Without
    # this, .ExitCode after the process is gone yields nothing at all --
    # a Get-Process object does not open the handle until something asks.
    try { $null = $helper.Handle } catch {
      Fail "could not open a handle to helper $($helper.Id): $($_.Exception.Message)"
    }
  }

  # The job object is what guarantees the helper cannot outlive the tray app.
  # Closing the tray app is therefore the assertion, not a cleanup step.
  $trayWindows = @(Get-ProcessWindow $tray.Id | Where-Object { $_.Class -eq $trayWindowClass })
  if ($trayWindows.Count -eq 0) {
    Show-FailureContext 'tray window not found'
    Fail "no window of class $trayWindowClass in pid $($tray.Id); the app cannot be asked to quit"
  }
  $trayWindow = $trayWindows[0].Handle
  if (-not [SyncSmoke.Win32]::PostMessageW($trayWindow, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)) {
    Show-FailureContext 'WM_CLOSE could not be posted'
    Fail 'could not post WM_CLOSE to the tray window'
  }
  if (-not $tray.WaitForExit(10000)) {
    Show-FailureContext 'app ignored WM_CLOSE'
    $tray.Kill()
    Fail 'app did not quit within ten seconds of WM_CLOSE'
  }

  for ($attempt = 0; $attempt -lt 50; $attempt++) {
    $survivors = @(Get-Process -Id $helperIds -ErrorAction SilentlyContinue)
    if ($survivors.Count -eq 0) { break }
    Start-Sleep -Milliseconds 100
  }
  $survivors = @(Get-Process -Id $helperIds -ErrorAction SilentlyContinue)
  if ($survivors.Count -ne 0) { Fail 'helper survived app quit' }

  # The helper must have SHUT DOWN, not merely stopped existing. syncd exits
  # 0 only after an orderly shutdown; the tray app's watchdog terminates it
  # with exit code 1 when the console control event never arrives. Without
  # this check a regression in the CTRL_BREAK_EVENT -> SIGBREAK path would
  # look identical to success here, just two seconds slower -- which is
  # exactly how that path came to be broken and unnoticed once already.
  foreach ($helper in $helperProcesses) {
    if (-not $helper.HasExited) { Fail "helper $($helper.Id) did not exit" }
    $code = $helper.ExitCode
    if ($null -eq $code) {
      Fail "helper $($helper.Id) exit code was unreadable; cannot tell a clean stop from a kill"
    }
    if ($code -ne 0) {
      Fail ("helper $($helper.Id) exited with $code; it was terminated " +
            'rather than shut down gracefully')
    }
  }

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
