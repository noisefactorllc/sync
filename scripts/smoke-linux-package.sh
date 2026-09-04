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
dpkg --remove noisedeck-sync
dpkg --purge noisedeck-sync
test ! -e /usr/bin/syncd
test ! -e /usr/bin/syncctl
