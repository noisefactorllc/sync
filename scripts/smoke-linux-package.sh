#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: smoke-linux-package.sh <archive>" >&2
  exit 2
fi
archive=$1
if pgrep -x syncd >/dev/null 2>&1; then
  echo "refusing smoke with an existing syncd process" >&2
  exit 1
fi
dpkg -i --force-depends "$archive"
test ! -e /etc/modprobe.d/noisedeck-sync-camera.conf
test ! -e /etc/modules-load.d/noisedeck-sync-camera.conf
if /usr/bin/syncd --definitely-invalid >/dev/null 2>&1; then exit 1; else test "$?" -eq 2; fi
if /usr/bin/syncctl --definitely-invalid >/dev/null 2>&1; then exit 1; else test "$?" -eq 2; fi
if pgrep -x syncd >/dev/null 2>&1; then
  echo "syncd remained running after invalid-option smoke" >&2
  exit 1
fi
# The packaged update command enrolls with the packaged archive keyring.
# The update helper refuses a trusted file under a group- or world-writable
# directory. Hosted Ubuntu runner images run `chmod -R 777 /usr/share`; restore
# the stock Ubuntu mode on the trusted directories first. On a stock host the
# directories are already 0755 and nothing changes.
for directory in /usr/share /usr/share/keyrings /usr/share/noisedeck-sync; do
  if [ -n "$(find "$directory" -maxdepth 0 -perm /022)" ]; then
    echo "smoke: removing group and world write from $directory" >&2
    chmod go-w "$directory"
  fi
done
update_user=${SUDO_USER:-$(id -un)}
/usr/bin/syncctl update enable --user "$update_user"
/usr/bin/syncctl update status | grep -q '"state": "enabled"'
/usr/bin/syncctl update disable
dpkg --remove noisedeck-sync
dpkg --purge noisedeck-sync
test ! -e /usr/bin/syncd
test ! -e /usr/bin/syncctl
