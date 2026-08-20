#Requires -Version 5.1
<#
.SYNOPSIS
  Stages, verifies, and packages the Sync Windows desktop preview.

.DESCRIPTION
  The Windows analogue of scripts/package-macos.sh. `bundle` stages Sync.exe,
  syncd.exe, the pinned SpoutLibrary.dll, the icon, and every non-system
  runtime dependency into <build-dir>/package/Sync. `installer` turns that
  staged directory into Sync-<version>-x64-Setup.exe with Inno Setup, and
  `zip` produces the portable archive.

  Like the macOS script, this produces an UNSIGNED app and installer.
  Authenticode signing and public publication belong to the Noise Factor
  release workflow so credentials never enter this public repository.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('bundle', 'installer', 'zip')]
  [string]$Mode,

  [Parameter(Mandatory = $true)]
  [string]$BuildDir,

  [Parameter(Mandatory = $true)]
  [string]$SourceDir,

  [Parameter(Mandatory = $true)]
  [string]$Version,

  # Absolute path to the pinned SpoutLibrary.dll. Required by `bundle`.
  [string]$SpoutLibrary = '',

  # Extra directory searched for runtime DLLs (the vcpkg or MSYS bin
  # directory holding libuv and OpenSSL). Mirrors package-macos.sh's
  # dylib search path.
  [string]$DependencySearchPath = '',

  # Directory the executables were actually written to. Single-config
  # generators (Ninja) put them at the build root; multi-config generators
  # (Visual Studio) put them in a per-configuration subdirectory, so CMake
  # passes $<TARGET_FILE_DIR:syncd> here rather than letting this guess.
  [string]$BinaryDir = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$message) {
  Write-Error "package-windows: $message"
  exit 1
}

function Resolve-AbsoluteDirectory([string]$path, [string]$label) {
  if ([string]::IsNullOrWhiteSpace($path)) { Fail "$label is required" }
  if (-not [System.IO.Path]::IsPathRooted($path)) { Fail "$label must be absolute: $path" }
  if (-not (Test-Path -LiteralPath $path -PathType Container)) {
    Fail "$label is not a directory: $path"
  }
  return (Resolve-Path -LiteralPath $path).ProviderPath
}

function Resolve-RequiredCommand([string]$name, [string]$hint) {
  $command = Get-Command $name -ErrorAction SilentlyContinue
  if ($null -eq $command) { Fail "missing required command: $name ($hint)" }
  return $command.Source
}

$BuildDir = Resolve-AbsoluteDirectory $BuildDir 'build directory'
$SourceDir = Resolve-AbsoluteDirectory $SourceDir 'source directory'
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
  Fail "version must be MAJOR.MINOR.PATCH, got '$Version'"
}

$packageDir = Join-Path $BuildDir 'package'
$bundleDir = Join-Path $packageDir 'Sync'
$stampPath = Join-Path $packageDir '.sync-bundle-complete'
$installerPath = Join-Path $packageDir "Sync-$Version-x64-Setup.exe"
$zipPath = Join-Path $packageDir "Sync-$Version-x64.zip"

if ($Mode -eq 'installer') {
  if (-not (Test-Path -LiteralPath $stampPath)) {
    Fail "bundle the app before building the installer: $stampPath is missing"
  }
  $iscc = Resolve-RequiredCommand 'ISCC' 'install Inno Setup 6 and put ISCC.exe on PATH'
  if (Test-Path -LiteralPath $installerPath) { Remove-Item -LiteralPath $installerPath -Force }
  & $iscc `
    "/DSyncVersion=$Version" `
    "/DSyncBundleDir=$bundleDir" `
    "/DSyncOutputDir=$packageDir" `
    "/DSyncSourceDir=$SourceDir" `
    (Join-Path $SourceDir 'packaging/windows/Sync.iss')
  if ($LASTEXITCODE -ne 0) { Fail "Inno Setup failed with exit code $LASTEXITCODE" }
  if (-not (Test-Path -LiteralPath $installerPath)) {
    Fail "Inno Setup did not produce $installerPath"
  }
  Write-Output $installerPath
  exit 0
}

if ($Mode -eq 'zip') {
  if (-not (Test-Path -LiteralPath $stampPath)) {
    Fail "bundle the app before building the archive: $stampPath is missing"
  }
  if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
  Compress-Archive -Path $bundleDir -DestinationPath $zipPath -CompressionLevel Optimal
  Write-Output $zipPath
  exit 0
}

# ---------------------------------------------------------------- bundle mode

$cmake = Resolve-RequiredCommand 'cmake' 'install CMake 3.21 or newer'

if ([string]::IsNullOrWhiteSpace($BinaryDir)) { $BinaryDir = $BuildDir }
$BinaryDir = Resolve-AbsoluteDirectory $BinaryDir 'binary directory'
$trayExe = Join-Path $BinaryDir 'Sync.exe'
$helperExe = Join-Path $BinaryDir 'syncd.exe'
foreach ($required in @($trayExe, $helperExe)) {
  if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
    Fail "build sync_menu and syncd before packaging: $required is missing"
  }
}

if ([string]::IsNullOrWhiteSpace($SpoutLibrary)) {
  Fail 'an absolute SpoutLibrary.dll path is required'
}
if (-not [System.IO.Path]::IsPathRooted($SpoutLibrary) -or
    -not (Test-Path -LiteralPath $SpoutLibrary -PathType Leaf)) {
  Fail "SpoutLibrary.dll must be an absolute path to an existing file: $SpoutLibrary"
}
$SpoutLibrary = (Resolve-Path -LiteralPath $SpoutLibrary).ProviderPath
if ([System.IO.Path]::GetFileName($SpoutLibrary) -ne 'SpoutLibrary.dll') {
  Fail "the pinned Spout module must be named SpoutLibrary.dll, got $SpoutLibrary"
}
if (-not [string]::IsNullOrWhiteSpace($DependencySearchPath)) {
  $DependencySearchPath = Resolve-AbsoluteDirectory $DependencySearchPath 'dependency search path'
}

if (Test-Path -LiteralPath $bundleDir) { Remove-Item -LiteralPath $bundleDir -Recurse -Force }
New-Item -ItemType Directory -Path $bundleDir -Force | Out-Null

Copy-Item -LiteralPath $trayExe -Destination (Join-Path $bundleDir 'Sync.exe')
Copy-Item -LiteralPath $helperExe -Destination (Join-Path $bundleDir 'syncd.exe')
# Spout is BSD-2-Clause, so unlike the NDI runtime it may be redistributed.
# It sits beside the executables because that is the only non-user-writable
# directory the provider's discovery search trusts.
Copy-Item -LiteralPath $SpoutLibrary -Destination (Join-Path $bundleDir 'SpoutLibrary.dll')
Copy-Item -LiteralPath (Join-Path $SourceDir 'LICENSE') `
  -Destination (Join-Path $bundleDir 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $SourceDir 'packaging/windows/Third-Party-Notices.txt') `
  -Destination (Join-Path $bundleDir 'Third-Party-Notices.txt')

# The build already rasterised the icon from packaging/Sync.svg and embedded
# it in both executables, so the bundle copies that exact file rather than
# producing a second one that could differ.
$builtIcon = Join-Path $BuildDir 'Sync.ico'
if (-not (Test-Path -LiteralPath $builtIcon -PathType Leaf)) {
  Fail ("the build did not produce $builtIcon; install ImageMagick (magick) " +
        'and re-run the build so the executables carry their icon')
}
Copy-Item -LiteralPath $builtIcon -Destination (Join-Path $bundleDir 'Sync.ico')

# Runtime dependencies come from CMake's own resolver rather than a bespoke
# scan, so the bundle carries exactly what the linker recorded.
$dependencyScript = Join-Path $SourceDir 'packaging/windows/bundle-dependencies.cmake'
& $cmake `
  "-DSYNC_BUNDLE_DIR=$bundleDir" `
  "-DSYNC_SEARCH_PATH=$DependencySearchPath" `
  -P $dependencyScript
if ($LASTEXITCODE -ne 0) { Fail "runtime dependency bundling failed with exit code $LASTEXITCODE" }

& (Join-Path $SourceDir 'scripts/verify-windows-bundle.ps1') -Bundle $bundleDir -ExpectedVersion $Version
if ($LASTEXITCODE -ne 0) { Fail "bundle verification failed with exit code $LASTEXITCODE" }

Set-Content -LiteralPath $stampPath -Value $Version -Encoding utf8
Write-Output $bundleDir
