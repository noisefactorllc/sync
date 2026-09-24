#!/usr/bin/env bash
set -euo pipefail

mode="${1:-}"
build_dir="${2:-}"
source_dir="${3:-}"
version="${4:-}"

if [[ "$mode" != "bundle" && "$mode" != "dmg" ]]; then
  echo "usage: $0 <bundle|dmg> <build-dir> <source-dir> <version> [Syphon.framework] [dylib-search-path]" >&2
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
  # The bundle's notices, not the source file: a bundle with the render
  # helper carries the generated Qt and helper section as well.
  cp "$bundle/Contents/Resources/Third-Party-Notices.txt" \
    "$dmg_root/Third-Party-Notices.txt"
  rm -f "$dmg"
  hdiutil create -quiet -volname "Sync Preview" -srcfolder "$dmg_root" \
    -ov -format UDZO "$dmg"
  rm -rf "$dmg_root"
  echo "$dmg"
  exit 0
fi

syphon_framework="${5:-}"
dylib_search_path="${6:-}"
for command in ditto dylibbundler install_name_tool rsvg-convert sips iconutil; do
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
if [[ -n "$dylib_search_path" &&
      ("$dylib_search_path" != /* || ! -d "$dylib_search_path") ]]; then
  echo "package-macos: dylib search path must be an absolute directory" >&2
  exit 1
fi
# The render helper is optional: a build without SYNC_BUILD_RENDER packages
# no sync-render and no Qt. When CMake supplies it, all three paths must be
# real, so a half-configured build cannot ship an app that cannot render.
render_binary="${SYNC_RENDER_BINARY:-}"
render_data="${SYNC_RENDER_DATA:-}"
macdeployqt="${SYNC_MACDEPLOYQT:-}"
if [[ -n "$render_binary$render_data$macdeployqt" ]]; then
  if [[ "$render_binary" != /* || ! -x "$render_binary" ]]; then
    echo "package-macos: SYNC_RENDER_BINARY must be an absolute executable" >&2
    exit 1
  fi
  if [[ "$render_data" != /* || ! -d "$render_data/effects" ||
        ! -d "$render_data/shaders" ]]; then
    echo "package-macos: SYNC_RENDER_DATA must hold effects/ and shaders/" >&2
    exit 1
  fi
  if [[ "$macdeployqt" != /* || ! -x "$macdeployqt" ]]; then
    echo "package-macos: SYNC_MACDEPLOYQT must be an absolute executable" >&2
    exit 1
  fi
  if [[ ! -d "${SYNC_RENDER_QT_SBOM_DIR:-}" || -z "${SYNC_RENDER_QT_MODULES:-}" ||
        -z "${SYNC_RENDER_NOTICE_COMPONENTS:-}" || ! -x "${SYNC_NODE:-}" ]]; then
    echo "package-macos: the render helper's notices need SYNC_RENDER_QT_SBOM_DIR," \
      "SYNC_RENDER_QT_MODULES, SYNC_RENDER_NOTICE_COMPONENTS and SYNC_NODE" >&2
    exit 1
  fi
fi

rm -rf "$bundle"
mkdir -p "$package_dir"
ditto "$build_dir/Sync.app" "$bundle"
mkdir -p "$bundle/Contents/Frameworks" "$bundle/Contents/Resources"
cp "$build_dir/syncd" "$bundle/Contents/MacOS/syncd"
chmod 0755 "$bundle/Contents/MacOS/Sync" "$bundle/Contents/MacOS/syncd"

# The Sync Camera system extension rides inside the app bundle. macOS finds
# it under Contents/Library/SystemExtensions when Sync.app asks to activate
# it, so the directory name must be the extension's bundle identifier.
extension="$bundle/Contents/Library/SystemExtensions/io.noisefactor.sync.camera.systemextension"
mkdir -p "$extension/Contents/MacOS"
cp "$build_dir/io.noisefactor.sync.camera" "$extension/Contents/MacOS/io.noisefactor.sync.camera"
chmod 0755 "$extension/Contents/MacOS/io.noisefactor.sync.camera"
cp "$build_dir/SyncCamera-Info.plist" "$extension/Contents/Info.plist"
ditto "$syphon_framework" "$bundle/Contents/Frameworks/Syphon.framework"
cp "$source_dir/LICENSE" "$bundle/Contents/Resources/LICENSE.txt"
cp "$source_dir/packaging/macos/Third-Party-Notices.txt" \
  "$bundle/Contents/Resources/Third-Party-Notices.txt"

iconset="$package_dir/Sync.iconset"
rm -rf "$iconset"
mkdir -p "$iconset"
for size in 16 32 128 256 512; do
  rsvg-convert -w "$size" -h "$size" "$source_dir/packaging/Sync.svg" \
    -o "$iconset/icon_${size}x${size}.png"
  doubled=$((size * 2))
  rsvg-convert -w "$doubled" -h "$doubled" \
    "$source_dir/packaging/Sync.svg" \
    -o "$iconset/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$iconset" -o "$bundle/Contents/Resources/Sync.icns"
rm -rf "$iconset"

for executable in "$bundle/Contents/MacOS/Sync" "$bundle/Contents/MacOS/syncd" \
    "$extension/Contents/MacOS/io.noisefactor.sync.camera"; do
  dylibbundler_arguments=(
    -b -cd -of -ns
    -x "$executable"
    -d "$bundle/Contents/Frameworks"
    -p '@executable_path/../Frameworks'
  )
  if [[ -n "$dylib_search_path" ]]; then
    dylibbundler_arguments+=(-s "$dylib_search_path")
  fi
  printf 'quit\n' | dylibbundler "${dylibbundler_arguments[@]}"
done

# dylibbundler copies Homebrew libraries but does not consistently rewrite
# syncd's load commands. Resolve these two direct dependencies to the copied
# regular files before bundle verification and signing.
for library in libuv libcrypto; do
  bundled_library="$(find "$bundle/Contents/Frameworks" -maxdepth 1 -type f \
    -name "$library*.dylib" -print -quit)"
  if [[ -z "$bundled_library" ]]; then
    echo "package-macos: missing bundled $library" >&2
    exit 1
  fi
  expected_dependency="@executable_path/../Frameworks/$(basename "$bundled_library")"
  while IFS= read -r dependency; do
    case "$(basename "$dependency")" in
      "$library"*.dylib)
        if [[ "$dependency" != "$expected_dependency" ]]; then
          install_name_tool -change "$dependency" "$expected_dependency" \
            "$bundle/Contents/MacOS/syncd"
        fi
        ;;
    esac
  done < <(otool -L "$bundle/Contents/MacOS/syncd" | awk 'NR > 1 { print $1 }')
done

# sync-render sits beside syncd, which finds it there. Its effect and shader
# data goes in Resources, the helper's last data-root candidate. macdeployqt
# then copies the Qt frameworks into Frameworks, the plugins into PlugIns,
# and writes Resources/qt.conf; the main executable links no Qt, so only the
# helper's dependencies are deployed. Signing happens later, in the release.
if [[ -n "$render_binary" ]]; then
  cp "$render_binary" "$bundle/Contents/MacOS/sync-render"
  chmod 0755 "$bundle/Contents/MacOS/sync-render"
  mkdir -p "$bundle/Contents/Resources/noisemaker"
  ditto "$render_data/effects" "$bundle/Contents/Resources/noisemaker/effects"
  ditto "$render_data/shaders" "$bundle/Contents/Resources/noisemaker/shaders"
  "$macdeployqt" "$bundle" \
    "-executable=$bundle/Contents/MacOS/sync-render" -verbose=1
  # Qt's own packages are universal (x86_64 and arm64). Sync ships for Apple
  # silicon only, so every library macdeployqt copied is thinned to arm64:
  # about half the size, and one architecture for the verifier and signing.
  while IFS= read -r -d '' library; do
    file -b "$library" | grep -q 'Mach-O universal' || continue
    lipo "$library" -thin arm64 -output "$library.arm64"
    mv -f "$library.arm64" "$library"
  done < <(find "$bundle/Contents/Frameworks" "$bundle/Contents/PlugIns" -type f -print0)
  # The helper's dependencies and every component of the shipped Qt, from
  # Qt's software bill of materials, with the full text of each license.
  notice_arguments=()
  IFS='|' read -r -a notice_components <<<"$SYNC_RENDER_NOTICE_COMPONENTS"
  for component in "${notice_components[@]}"; do
    notice_arguments+=(--component "$component")
  done
  {
    echo
    "$SYNC_NODE" "$source_dir/scripts/render-notices.mjs" \
      --sbom-dir "$SYNC_RENDER_QT_SBOM_DIR" --modules "$SYNC_RENDER_QT_MODULES" \
      --spdx-texts "$source_dir/packaging/licenses/spdx" "${notice_arguments[@]}"
  } >>"$bundle/Contents/Resources/Third-Party-Notices.txt"
fi

"$source_dir/scripts/verify-macos-bundle.sh" "$bundle" "$version"
touch "$package_dir/.sync-bundle-complete"
echo "$bundle"
