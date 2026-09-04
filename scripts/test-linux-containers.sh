#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH='' cd -- "$script_dir/.." && pwd)
case "$source_dir" in
  /) echo "refusing a root source directory" >&2; exit 2 ;;
esac

version=${SYNC_VERSION:-}
case "$version" in
  '') echo "SYNC_VERSION must be supplied by the invoking CI job or test command" >&2; exit 2 ;;
  *[!0-9A-Za-z.+:~-]*) echo "SYNC_VERSION contains an unsafe package character" >&2; exit 2 ;;
esac

output_dir="$source_dir/build-package/package"
output_tmp="$source_dir/build-package/.Sync-$version-linux-amd64.deb.tmp"
output_archive="$output_dir/Sync-$version-linux-amd64.deb"
mkdir -p "$output_dir"
rm -f "$output_tmp" "$output_archive"

arm_image=noisedeck-sync-test:ubuntu24.04-arm64
amd_image=noisedeck-sync-test:ubuntu24.04-amd64
arm_volume=noisedeck-sync-arm64-build-$$
amd_volume=noisedeck-sync-amd64-build-$$
asan_volume=noisedeck-sync-amd64-asan-$$
package_container=
cleanup() {
  if [ -n "$package_container" ]; then docker rm -f "$package_container" >/dev/null 2>&1 || true; fi
  docker volume rm "$arm_volume" "$amd_volume" "$asan_volume" >/dev/null 2>&1 || true
  rm -f "$output_tmp"
}
trap cleanup EXIT HUP INT TERM

docker build --platform linux/arm64 -t "$arm_image" \
  -f "$source_dir/test/linux/ubuntu-24.04.Dockerfile" "$source_dir"
docker volume create "$arm_volume" >/dev/null
docker run --rm --platform linux/arm64 \
  -v "$source_dir:/src:ro" -v "$arm_volume:/build" "$arm_image" sh -ec '
    cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Werror"
    cmake --build /build --parallel 2
    ctest --test-dir /build --output-on-failure
    cd /src
    node --test test/browser/*.test.js
  '

docker build --platform linux/amd64 -t "$amd_image" \
  -f "$source_dir/test/linux/ubuntu-24.04.Dockerfile" "$source_dir"
docker volume create "$amd_volume" >/dev/null
docker run --rm --platform linux/amd64 \
  -v "$source_dir:/src:ro" -v "$amd_volume:/build" "$amd_image" sh -ec "
    cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \\
      -DSYNC_PRODUCT_VERSION='$version' \\
      -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
    cmake --build /build --parallel 2
    ctest --test-dir /build --output-on-failure
    cd /src
    node --test test/browser/*.test.js
    SYNC_VERSION='$version' SYNC_DAEMON_PATH=/build/syncd \\
      node --test test/integration/loopback.test.js
    SYNC_DAEMON_PATH=/build/syncd SYNC_CTL_PATH=/build/syncctl \\
      node --test test/integration/linux-daemon-control.test.js
    cmake --build /build --target sync_linux_deb --parallel 2
    SYNC_PACKAGE_DIR=/build/package node --test test/packaging/linux-package.test.js
    scripts/verify-linux-package.sh /build/package/Sync-$version-linux-amd64.deb '$version'
  "

docker volume create "$asan_volume" >/dev/null
docker run --rm --platform linux/amd64 \
  -v "$source_dir:/src:ro" -v "$asan_volume:/build" "$amd_image" sh -ec '
    cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
    cmake --build /build --parallel 2
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
      ctest --test-dir /build --output-on-failure
  '

package_container=$(docker create --platform linux/amd64 \
  -v "$amd_volume:/build:ro" "$amd_image" true)
docker cp "$package_container:/build/package/Sync-$version-linux-amd64.deb" \
  "$output_tmp"
docker rm "$package_container" >/dev/null
package_container=
mv -f "$output_tmp" "$output_archive"

docker run --rm --platform linux/amd64 \
  -v "$output_archive:/tmp/Sync.deb:ro" \
  -v "$source_dir/scripts/smoke-linux-package.sh:/tmp/smoke.sh:ro" \
  "$amd_image" sh /tmp/smoke.sh /tmp/Sync.deb

printf '%s\n' "$output_archive"
