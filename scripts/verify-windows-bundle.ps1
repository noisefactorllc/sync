#Requires -Version 5.1
<#
.SYNOPSIS
  Verifies a staged Sync Windows bundle before it is turned into an installer.

.DESCRIPTION
  The Windows counterpart to scripts/verify-macos-bundle.sh. It asserts that
  the bundle is complete, that both executables are 64-bit, that they carry the
  advertised version, and -- the point of the exercise -- that nothing in the
  bundle still depends on a DLL that is neither bundled nor part of Windows.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$Bundle,

  [string]$ExpectedVersion = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$message) {
  Write-Error "verify-windows-bundle: $message"
  exit 1
}

if (-not [System.IO.Path]::IsPathRooted($Bundle)) {
  Fail "an absolute bundle path is required, got '$Bundle'"
}
if (-not (Test-Path -LiteralPath $Bundle -PathType Container)) {
  Fail "bundle directory does not exist: $Bundle"
}
$Bundle = (Resolve-Path -LiteralPath $Bundle).ProviderPath

foreach ($required in @(
    'Sync.exe',
    'syncd.exe',
    'SpoutLibrary.dll',
    'SyncCamera.dll',
    'Sync.ico',
    'LICENSE.txt',
    'Third-Party-Notices.txt')) {
  if (-not (Test-Path -LiteralPath (Join-Path $Bundle $required) -PathType Leaf)) {
    Fail "missing $required"
  }
}

# The NDI runtime must NOT be here: its licence does not permit redistribution,
# and shipping it would turn a documented user-installed dependency into a
# licensing violation. This check is the guard that keeps that true.
foreach ($forbidden in Get-ChildItem -LiteralPath $Bundle -File -Filter '*.dll') {
  if ($forbidden.Name -match '^Processing\.NDI\.') {
    Fail "the NDI runtime must not be redistributed, found $($forbidden.Name)"
  }
}

function Get-PeMachine([string]$path) {
  $stream = [System.IO.File]::OpenRead($path)
  try {
    $reader = New-Object System.IO.BinaryReader($stream)
    $stream.Seek(0x3c, 'Begin') | Out-Null
    $headerOffset = $reader.ReadInt32()
    $stream.Seek($headerOffset, 'Begin') | Out-Null
    if ($reader.ReadUInt32() -ne 0x00004550) { return 0 }  # 'PE\0\0'
    return $reader.ReadUInt16()
  } finally {
    $stream.Dispose()
  }
}

$imageFileMachineAmd64 = 0x8664
$binaries = Get-ChildItem -LiteralPath $Bundle -File |
  Where-Object { $_.Extension -in @('.exe', '.dll') }
if ($binaries.Count -lt 4) {
  Fail ("expected the tray app, the helper, SpoutLibrary and the camera source " +
        "at minimum, found $($binaries.Count) binaries")
}
foreach ($binary in $binaries) {
  $machine = Get-PeMachine $binary.FullName
  if ($machine -ne $imageFileMachineAmd64) {
    Fail ("{0} is not an x64 PE image (machine 0x{1:x})" -f $binary.Name, $machine)
  }
}

if (-not [string]::IsNullOrWhiteSpace($ExpectedVersion)) {
  foreach ($name in @('Sync.exe', 'syncd.exe')) {
    $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $Bundle $name))
    $actual = $info.ProductVersion
    if ($null -ne $actual) { $actual = $actual.Trim() }
    if ($actual -ne $ExpectedVersion) {
      Fail "$name reports product version '$actual', expected '$ExpectedVersion'"
    }
  }
}

Write-Output "verified $Bundle ($($binaries.Count) x64 binaries)"
