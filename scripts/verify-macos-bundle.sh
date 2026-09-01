#!/usr/bin/env bash
set -euo pipefail

bundle="${1:-}"
expected_version="${2:-}"
if [[ -z "$bundle" || "$bundle" != /* || ! -d "$bundle" ]]; then
  echo "verify-macos-bundle: absolute Sync.app path is required" >&2
  exit 2
fi

contents="$bundle/Contents"
info="$contents/Info.plist"
for required in \
  "$contents/MacOS/Sync" \
  "$contents/MacOS/syncd" \
  "$contents/Frameworks/Syphon.framework" \
  "$contents/Library/SystemExtensions/io.noisefactor.sync.camera.systemextension/Contents/MacOS/io.noisefactor.sync.camera" \
  "$contents/Library/SystemExtensions/io.noisefactor.sync.camera.systemextension/Contents/Info.plist" \
  "$contents/Resources/Sync.icns" \
  "$contents/Resources/LICENSE.txt" \
  "$contents/Resources/Third-Party-Notices.txt"; do
  if [[ ! -e "$required" ]]; then
    echo "verify-macos-bundle: missing $required" >&2
    exit 1
  fi
done

syphon_binary="$contents/Frameworks/Syphon.framework/Syphon"
if ! nm -gU "$syphon_binary" 2>/dev/null |
    awk '$NF == "_OBJC_CLASS_$_SyphonMetalServer" { found = 1 }
         END { exit !found }'; then
  echo "verify-macos-bundle: $syphon_binary does not export SyphonMetalServer" >&2
  exit 1
fi

plutil -lint "$info" >/dev/null
identifier="$(plutil -extract CFBundleIdentifier raw -o - "$info")"
version="$(plutil -extract CFBundleShortVersionString raw -o - "$info")"
agent="$(plutil -extract LSUIElement raw -o - "$info")"
minimum_system="$(plutil -extract LSMinimumSystemVersion raw -o - "$info")"
if [[ "$identifier" != "io.noisefactor.sync" || "$agent" != "true" ]]; then
  echo "verify-macos-bundle: invalid identity or LSUIElement metadata" >&2
  exit 1
fi
if [[ -n "$expected_version" && "$version" != "$expected_version" ]]; then
  echo "verify-macos-bundle: expected version $expected_version, found $version" >&2
  exit 1
fi

mach_count=0
while IFS= read -r -d '' candidate; do
  if [[ ! -f "$candidate" ]] || ! file -b "$candidate" | grep -q 'Mach-O'; then
    continue
  fi
  mach_count=$((mach_count + 1))
  if ! lipo -archs "$candidate" | tr ' ' '\n' | grep -qx arm64; then
    echo "verify-macos-bundle: $candidate does not contain arm64" >&2
    exit 1
  fi
  deployment_target="$(otool -l "$candidate" | awk '
    $1 == "cmd" { command = $2 }
    command == "LC_BUILD_VERSION" && $1 == "minos" { print $2; exit }
    command == "LC_VERSION_MIN_MACOSX" && $1 == "version" { print $2; exit }
  ')"
  if [[ -z "$deployment_target" ]]; then
    echo "verify-macos-bundle: $candidate has neither LC_BUILD_VERSION nor LC_VERSION_MIN_MACOSX" >&2
    exit 1
  fi
  if ! awk -v actual="$deployment_target" -v maximum="$minimum_system" '
    BEGIN {
      split(actual, actual_parts, ".")
      split(maximum, maximum_parts, ".")
      for (part_index = 1; part_index <= 3; part_index++) {
        actual_part = actual_parts[part_index] + 0
        maximum_part = maximum_parts[part_index] + 0
        if (actual_part < maximum_part) exit 0
        if (actual_part > maximum_part) exit 1
      }
      exit 0
    }
  '; then
    echo "verify-macos-bundle: $candidate targets macOS $deployment_target, newer than the bundle minimum $minimum_system" >&2
    exit 1
  fi
  while IFS= read -r dependency; do
    dependency="${dependency#"${dependency%%[![:space:]]*}"}"
    dependency="${dependency%% *}"
    [[ -z "$dependency" ]] && continue
    case "$dependency" in
      /System/Library/*|/usr/lib/*|@rpath/*|@loader_path/*|@executable_path/*) ;;
      *)
        echo "verify-macos-bundle: unresolved dependency $dependency in $candidate" >&2
        exit 1
        ;;
    esac
  done < <(otool -L "$candidate" | tail -n +2)
done < <(find "$contents" -type f -print0)

if (( mach_count < 4 )); then
  echo "verify-macos-bundle: expected app, helper, camera extension, and framework Mach-O files" >&2
  exit 1
fi

echo "verified $bundle ($version, $mach_count Mach-O files)"
