#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: package-linux.sh <build-dir> <source-dir> <version> <architecture>" >&2
  exit 2
fi

build_dir=$1
source_dir=$2
version=$3
architecture=$4
case "$build_dir:$source_dir" in
  /*:/*) ;;
  *) echo "build and source directories must be absolute" >&2; exit 2 ;;
esac
if [ "$build_dir" = / ] || [ "$source_dir" = / ] || [ "$architecture" != amd64 ]; then
  echo "release packaging requires explicit non-root paths and amd64" >&2
  exit 2
fi
case "$version" in
  ''|*[!0-9A-Za-z.+:~-]*) echo "invalid package version" >&2; exit 2 ;;
esac
source_date_epoch=${SOURCE_DATE_EPOCH:-0}
case "$source_date_epoch" in
  ''|*[!0-9]*) echo "SOURCE_DATE_EPOCH must be an unsigned integer" >&2; exit 2 ;;
esac
export SOURCE_DATE_EPOCH="$source_date_epoch"

stage="$build_dir/linux-package-stage"
package_dir="$build_dir/package"
archive="$package_dir/Sync-$version-linux-amd64.deb"
temporary="$package_dir/.Sync-$version-linux-amd64.deb.tmp"
rm -rf "$stage"
mkdir -p "$stage/DEBIAN" "$stage/usr/bin" \
  "$stage/usr/lib/systemd/user" "$stage/usr/lib/udev/rules.d" \
  "$stage/usr/share/noisedeck-sync" "$stage/usr/share/doc/noisedeck-sync" \
  "$package_dir"

install -m 0755 "$build_dir/syncd" "$stage/usr/bin/syncd"
install -m 0755 "$build_dir/syncctl" "$stage/usr/bin/syncctl"
strip --strip-unneeded "$stage/usr/bin/syncd" "$stage/usr/bin/syncctl"
install -m 0644 "$source_dir/packaging/linux/noisedeck-sync.service" \
  "$stage/usr/lib/systemd/user/noisedeck-sync.service"
install -m 0644 "$source_dir/packaging/linux/70-noisedeck-sync-camera.rules" \
  "$stage/usr/lib/udev/rules.d/70-noisedeck-sync-camera.rules"
install -m 0644 "$source_dir/packaging/linux/70-noisedeck-sync-camera.rules" \
  "$stage/usr/share/noisedeck-sync/70-noisedeck-sync-camera.rules"
install -m 0644 "$source_dir/packaging/linux/noisedeck-sync-camera.modprobe" \
  "$stage/usr/share/noisedeck-sync/noisedeck-sync-camera.modprobe"
install -m 0644 "$source_dir/packaging/linux/noisedeck-sync-camera.modules-load" \
  "$stage/usr/share/noisedeck-sync/noisedeck-sync-camera.modules-load"
install -m 0644 "$source_dir/LICENSE" \
  "$stage/usr/share/doc/noisedeck-sync/copyright"
install -m 0644 "$source_dir/packaging/linux/Third-Party-Notices.txt" \
  "$stage/usr/share/doc/noisedeck-sync/Third-Party-Notices.txt"
install -m 0755 "$source_dir/packaging/linux/postrm" "$stage/DEBIAN/postrm"
sed -e "s/@VERSION@/$version/g" -e "s/@ARCHITECTURE@/$architecture/g" \
  "$source_dir/packaging/linux/control.in" > "$stage/DEBIAN/control"
chmod 0644 "$stage/DEBIAN/control"

find "$stage" -exec touch -h -d "@$source_date_epoch" {} +
rm -f "$temporary"
dpkg-deb --build --root-owner-group "$stage" "$temporary"
touch -h -d "@$source_date_epoch" "$temporary"
mv -f "$temporary" "$archive"
printf '%s\n' "$archive"
