#!/usr/bin/env bash
set -euo pipefail

bundle="${1:-}"
if [[ -z "$bundle" || "$bundle" != /* || ! -d "$bundle" ]]; then
  echo "smoke-macos-app: absolute Sync.app path is required" >&2
  exit 2
fi
if lsof -nP -iTCP:53979 -sTCP:LISTEN >/dev/null 2>&1; then
  echo "smoke-macos-app: TCP 53979 is already occupied" >&2
  exit 1
fi

app_pid=""
helper_pid=""
notice_default_existed=false
notice_default_previous=""
restore_notice_default() {
  if [[ "$notice_default_existed" = true ]]; then
    defaults write io.noisefactor.sync SyncPreviewNoticeShown \
      -bool "$notice_default_previous" >/dev/null 2>&1 || true
  else
    defaults delete io.noisefactor.sync SyncPreviewNoticeShown >/dev/null 2>&1 || true
  fi
}
cleanup() {
  [[ -n "$helper_pid" ]] && kill "$helper_pid" >/dev/null 2>&1 || true
  [[ -n "$app_pid" ]] && kill "$app_pid" >/dev/null 2>&1 || true
  restore_notice_default
}
trap cleanup EXIT

# This runs on developer machines as well as CI, so the preview-notice default
# is borrowed rather than taken: whatever the user had is put back on exit.
if notice_default_previous="$(defaults read io.noisefactor.sync \
    SyncPreviewNoticeShown 2>/dev/null)"; then
  notice_default_existed=true
fi
defaults write io.noisefactor.sync SyncPreviewNoticeShown -bool true
"$bundle/Contents/MacOS/Sync" >/tmp/sync-preview-app.out 2>/tmp/sync-preview-app.err &
app_pid=$!

health=""
for _ in $(seq 1 100); do
  if ! kill -0 "$app_pid" >/dev/null 2>&1; then
    echo "smoke-macos-app: Sync exited before becoming healthy" >&2
    sed -n '1,120p' /tmp/sync-preview-app.err >&2 || true
    exit 1
  fi
  health="$(curl -fsS --max-time 0.5 http://127.0.0.1:53979/status 2>/dev/null || true)"
  if [[ -n "$health" ]]; then break; fi
  sleep 0.1
done
if [[ -z "$health" ]]; then
  echo "smoke-macos-app: Sync did not become healthy" >&2
  exit 1
fi
if [[ "$(jq -r '.product' <<<"$health")" != "Sync" ||
      "$(jq -r '.status' <<<"$health")" != "ok" ||
      "$(jq -r '.capabilities.providers[] | select(.id == "syphon") | .available' \
          <<<"$health")" != "true" ]]; then
  echo "smoke-macos-app: bundled Syphon is not available: $health" >&2
  exit 1
fi

helper_pid="$(pgrep -P "$app_pid" -f "$bundle/Contents/MacOS/syncd" | head -1 || true)"
if [[ -z "$helper_pid" ]]; then
  echo "smoke-macos-app: managed helper was not found" >&2
  exit 1
fi

# `tell application ... to quit` LAUNCHES the target when it is not already
# running, so a app that died early would be resurrected here and the quit
# assertions below would test a process this script started by accident.
if ! kill -0 "$app_pid" >/dev/null 2>&1; then
  echo "smoke-macos-app: Sync exited before the quit check" >&2
  exit 1
fi
osascript -e 'tell application id "io.noisefactor.sync" to quit'
for _ in $(seq 1 50); do
  if ! kill -0 "$app_pid" >/dev/null 2>&1; then break; fi
  sleep 0.1
done
if kill -0 "$app_pid" >/dev/null 2>&1; then
  echo "smoke-macos-app: app did not quit within five seconds" >&2
  exit 1
fi
app_pid=""
if kill -0 "$helper_pid" >/dev/null 2>&1; then
  echo "smoke-macos-app: helper survived app quit" >&2
  exit 1
fi
helper_pid=""
trap - EXIT
echo "Sync app lifecycle smoke passed"
