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
if printf '%s\n' "$contents" | grep -q './usr/libexec/noisedeck-sync-update$'; then
  for required in ./usr/share/keyrings/noisedeck-sync-archive-keyring.gpg \
                  ./usr/share/noisedeck-sync/apt-key-fingerprint; do
    if ! printf '%s\n' "$contents" | grep -q "$required\$"; then
      echo "update helper without $required" >&2
      exit 1
    fi
  done
fi
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
if [ -e "$temporary/usr/libexec/noisedeck-sync-update" ]; then
  fingerprint=$(cat "$temporary/usr/share/noisedeck-sync/apt-key-fingerprint")
  printf '%s\n' "$fingerprint" | grep -Eqx '[A-F0-9]{40}'
  if command -v gpg >/dev/null 2>&1; then
    primary=$(gpg --batch --no-options --homedir "$temporary" --with-colons --show-keys \
      "$temporary/usr/share/keyrings/noisedeck-sync-archive-keyring.gpg" 2>/dev/null |
      awk -F: '$1=="pub" {p=1; next} $1=="fpr" && p {print $10; p=0}')
    if [ "$primary" != "$fingerprint" ]; then
      echo "packaged keyring fingerprint mismatch" >&2
      exit 1
    fi
  fi
fi
