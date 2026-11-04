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

# A build with the render helper sets SYNC_EXPECT_RENDER=1. The helper, its
# data and the Qt it links must then all be in the bundle: syncd starts
# sync-render from beside itself, and Qt needs its Windows platform plugin.
# The helper's Seance connection is wss://, and Qt has no TLS without a
# backend plugin: the system's (Schannel) needs no OpenSSL. Shared WebP
# images need qtimageformats' WebP plugin.
if ($env:SYNC_EXPECT_RENDER -eq '1') {
  foreach ($required in @('sync-render.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6OpenGL.dll',
                          'Qt6WebSockets.dll', 'Qt6Multimedia.dll', 'platforms/qwindows.dll',
                          'imageformats/qgif.dll', 'imageformats/qjpeg.dll',
                          'tls/qschannelbackend.dll', 'imageformats/qwebp.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $Bundle $required) -PathType Leaf)) {
      Fail "render helper bundle is missing $required"
    }
  }
  foreach ($directory in @('noisemaker/effects', 'noisemaker/shaders')) {
    if (-not (Test-Path -LiteralPath (Join-Path $Bundle $directory) -PathType Container)) {
      Fail "render helper bundle is missing $directory"
    }
  }
  $notices = Get-Content -LiteralPath (Join-Path $Bundle 'Third-Party-Notices.txt')
  if (-not ($notices -contains 'Sync render helper (sync-render)') -or
      -not ($notices -contains 'LGPL-3.0-only')) {
    Fail 'Third-Party-Notices.txt lacks the render helper and Qt notices'
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

# Every DLL a bundled binary imports, directly or delay-loaded, must be in the
# bundle or be part of Windows, or the installed app fails to start on a
# machine that lacks it. "Part of Windows" is an API set name, or a file in
# System32 other than the Visual C++ runtime. A build machine has that runtime
# in System32 too, and Windows signs it there like its own files, but its
# version resource names Visual Studio, and a user's machine may not have it.
Add-Type -Namespace SyncVerify -Name PeImports -MemberDefinition @'
static uint ToOffset(byte[] image, int sections, int count, uint rva) {
  for (int i = 0; i < count; i++) {
    int section = sections + i * 40;
    uint size = System.BitConverter.ToUInt32(image, section + 8);
    uint address = System.BitConverter.ToUInt32(image, section + 12);
    uint raw = System.BitConverter.ToUInt32(image, section + 20);
    if (rva >= address && rva < address + System.Math.Max(size, System.BitConverter.ToUInt32(image, section + 16))) {
      return rva - address + raw;
    }
  }
  throw new System.IO.InvalidDataException("RVA outside every section");
}

static string ReadName(byte[] image, uint offset) {
  int end = (int)offset;
  while (end < image.Length && image[end] != 0) end++;
  return System.Text.Encoding.ASCII.GetString(image, (int)offset, end - (int)offset);
}

public static string[] Read(string path) {
  byte[] image = System.IO.File.ReadAllBytes(path);
  int header = System.BitConverter.ToInt32(image, 0x3c);
  int optional = header + 24;
  bool plus = System.BitConverter.ToUInt16(image, optional) == 0x20b;
  int directories = optional + (plus ? 112 : 96);
  int count = System.BitConverter.ToUInt16(image, header + 6);
  int sections = optional + System.BitConverter.ToUInt16(image, header + 20);
  var names = new System.Collections.Generic.List<string>();
  // Data directory 1 is the import table: 20-byte descriptors, the name
  // RVA at offset 12, ending at an all-zero descriptor.
  uint imports = System.BitConverter.ToUInt32(image, directories + 8);
  if (imports != 0) {
    for (uint d = ToOffset(image, sections, count, imports); ; d += 20) {
      uint name = System.BitConverter.ToUInt32(image, (int)d + 12);
      if (name == 0) break;
      names.Add(ReadName(image, ToOffset(image, sections, count, name)));
    }
  }
  // Data directory 13 is the delay-load table: 32-byte descriptors, the
  // name RVA at offset 4, ending at an all-zero descriptor.
  uint delayed = System.BitConverter.ToUInt32(image, directories + 13 * 8);
  if (delayed != 0) {
    for (uint d = ToOffset(image, sections, count, delayed); ; d += 32) {
      uint name = System.BitConverter.ToUInt32(image, (int)d + 4);
      if (name == 0) break;
      names.Add(ReadName(image, ToOffset(image, sections, count, name)));
    }
  }
  return names.ToArray();
}
'@

$bundled = @{}
foreach ($file in Get-ChildItem -LiteralPath $Bundle -File -Recurse |
           Where-Object { $_.Extension -in @('.exe', '.dll') }) {
  $bundled[$file.Name.ToLowerInvariant()] = $true
}
$windowsDirectory = Join-Path $env:SystemRoot 'System32'
$partOfWindows = @{}
$missing = New-Object System.Collections.ArrayList
foreach ($binary in Get-ChildItem -LiteralPath $Bundle -File -Recurse |
         Where-Object { $_.Extension -in @('.exe', '.dll') }) {
  foreach ($import in [SyncVerify.PeImports]::Read($binary.FullName)) {
    $name = $import.ToLowerInvariant()
    if ($bundled.ContainsKey($name)) { continue }
    if ($name -match '^(api-ms-win-|ext-ms-)') { continue }
    if (-not $partOfWindows.ContainsKey($name)) {
      $systemFile = Join-Path $windowsDirectory $name
      $partOfWindows[$name] = (Test-Path -LiteralPath $systemFile -PathType Leaf) -and
        ([string][System.Diagnostics.FileVersionInfo]::GetVersionInfo($systemFile).ProductName -notmatch
          'Visual Studio')
    }
    if (-not $partOfWindows[$name]) {
      $relative = $binary.FullName.Substring($Bundle.Length).TrimStart('\', '/')
      [void]$missing.Add("$relative imports $import")
    }
  }
}
if ($missing.Count -gt 0) {
  Fail ("these imports are neither in the bundle nor part of Windows: " +
        ($missing -join '; '))
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
