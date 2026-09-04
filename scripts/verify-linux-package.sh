#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: verify-linux-package.sh <archive> <version>" >&2
  exit 2
fi
archive=$1
version=$2
test "$(dpkg-deb --field "$archive" Package)" = noisedeck-sync
test "$(dpkg-deb --field "$archive" Version)" = "$version"
test "$(dpkg-deb --field "$archive" Architecture)" = amd64
contents=$(dpkg-deb --contents "$archive")
printf '%s\n' "$contents" | grep -q './usr/bin/syncd$'
printf '%s\n' "$contents" | grep -q './usr/bin/syncctl$'
if printf '%s\n' "$contents" | grep -Eqi ' -> |libndi|\.ko($|[[:space:]])|\.desktop|autostart'; then
  echo "forbidden payload in Linux package" >&2
  exit 1
fi
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
dpkg-deb --extract "$archive" "$temporary"
file "$temporary/usr/bin/syncd" | grep -q 'ELF 64-bit LSB.*x86-64'
file "$temporary/usr/bin/syncctl" | grep -q 'ELF 64-bit LSB.*x86-64'
! grep -q SYMLINK "$temporary/usr/lib/udev/rules.d/70-noisedeck-sync-camera.rules"
