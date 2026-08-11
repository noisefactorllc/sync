#!/usr/bin/env bash
set -euo pipefail

mode="${1:-}"
build_dir="${2:-}"
source_dir="${3:-}"
version="${4:-}"

if [[ "$mode" != "bundle" && "$mode" != "dmg" ]]; then
  echo "usage: $0 <bundle|dmg> <build-dir> <source-dir> <version> [Syphon.framework]" >&2
  exit 2
fi
if [[ -z "$build_dir" || -z "$source_dir" || -z "$version" ||
      "$build_dir" != /* || "$source_dir" != /* || "$build_dir" == "/" ]]; then
  echo "package-macos: absolute, bounded build and source paths are required" >&2
  exit 2
fi

package_dir="$build_dir/package"
bundle="$package_dir/Sync.app"
dmg="$package_dir/Sync-${version}-arm64.dmg"

if [[ "$mode" == "dmg" ]]; then
  if [[ ! -d "$bundle" ]]; then
    echo "package-macos: bundle does not exist: $bundle" >&2
    exit 1
  fi
  dmg_root="$package_dir/dmg-root"
  rm -rf "$dmg_root"
  mkdir -p "$dmg_root"
  ditto "$bundle" "$dmg_root/Sync.app"
  ln -s /Applications "$dmg_root/Applications"
  cp "$source_dir/LICENSE" "$dmg_root/LICENSE.txt"
  cp "$source_dir/packaging/macos/Third-Party-Notices.txt" \
    "$dmg_root/Third-Party-Notices.txt"
  rm -f "$dmg"
  hdiutil create -quiet -volname "Sync Preview" -srcfolder "$dmg_root" \
    -ov -format UDZO "$dmg"
  rm -rf "$dmg_root"
  echo "$dmg"
  exit 0
fi

syphon_framework="${5:-}"
for command in ditto dylibbundler rsvg-convert sips iconutil; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "package-macos: missing required command: $command" >&2
    exit 1
  fi
done
if [[ ! -d "$build_dir/Sync.app" || ! -x "$build_dir/syncd" ]]; then
  echo "package-macos: build sync_menu and syncd before packaging" >&2
  exit 1
fi
if [[ -z "$syphon_framework" || "$syphon_framework" != /* ||
      ! -d "$syphon_framework" ]]; then
  echo "package-macos: an absolute Syphon.framework path is required" >&2
  exit 1
fi

rm -rf "$bundle"
mkdir -p "$package_dir"
ditto "$build_dir/Sync.app" "$bundle"
mkdir -p "$bundle/Contents/Frameworks" "$bundle/Contents/Resources"
cp "$build_dir/syncd" "$bundle/Contents/MacOS/syncd"
chmod 0755 "$bundle/Contents/MacOS/Sync" "$bundle/Contents/MacOS/syncd"
ditto "$syphon_framework" "$bundle/Contents/Frameworks/Syphon.framework"
cp "$source_dir/LICENSE" "$bundle/Contents/Resources/LICENSE.txt"
cp "$source_dir/packaging/macos/Third-Party-Notices.txt" \
  "$bundle/Contents/Resources/Third-Party-Notices.txt"

iconset="$package_dir/Sync.iconset"
rm -rf "$iconset"
mkdir -p "$iconset"
for size in 16 32 128 256 512; do
  rsvg-convert -w "$size" -h "$size" "$source_dir/packaging/macos/Sync.svg" \
    -o "$iconset/icon_${size}x${size}.png"
  doubled=$((size * 2))
  rsvg-convert -w "$doubled" -h "$doubled" \
    "$source_dir/packaging/macos/Sync.svg" \
    -o "$iconset/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$iconset" -o "$bundle/Contents/Resources/Sync.icns"
rm -rf "$iconset"

for executable in "$bundle/Contents/MacOS/Sync" "$bundle/Contents/MacOS/syncd"; do
  dylibbundler -b -cd -of -ns -x "$executable" \
    -d "$bundle/Contents/Frameworks" \
    -p '@executable_path/../Frameworks'
done

"$source_dir/scripts/verify-macos-bundle.sh" "$bundle" "$version"
touch "$package_dir/.sync-bundle-complete"
echo "$bundle"
