#!/usr/bin/env bash
# Real driver qualification using an isolated software source; no physical inputs.
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo 'usage: test-linux-audio.sh <build-dir> <artifact-dir> [jack|pipewire]' >&2
  exit 2
fi
build_dir=$(cd "$1" && pwd)
mkdir -p "$2"
artifact_dir=$(cd "$2" && pwd)
mode=${3:-jack}
case "$mode" in jack|pipewire) ;; *) echo 'unknown audio backend' >&2; exit 2 ;; esac
source_dir=$(cd "$(dirname "$0")/.." && pwd)
test -x "$build_dir/sync_audio_native_probe"
runtime_dir=$(mktemp -d)
server_pid=
source_pid=
cleanup() {
  for pid in "$source_pid" "$server_pid"; do
    if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  done
  rm -rf "$runtime_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

c++ -std=c++20 -Wall -Wextra -Werror \
  "$source_dir/test/linux/jack-audio-source.cpp" -ljack -pthread \
  -o "$runtime_dir/source"
export JACK_NO_START_SERVER=1
wrapper=()
if [[ "$mode" == jack ]]; then
  export JACK_DEFAULT_SERVER="sync-audio-qualification-$$"
  jackd --no-realtime --name "$JACK_DEFAULT_SERVER" --port-max 128 \
    -d dummy -r 48000 -p 1024 > "$artifact_dir/server.log" 2>&1 &
  server_pid=$!
else
  export XDG_RUNTIME_DIR="$runtime_dir"
  export PIPEWIRE_RUNTIME_DIR="$runtime_dir"
  export PIPEWIRE_REMOTE=pipewire-0
  export PIPEWIRE_QUANTUM=1024/48000
  wrapper=(pw-jack)
  pipewire > "$artifact_dir/server.log" 2>&1 &
  server_pid=$!
fi

# Opening with JackNoStartServer makes startup retries safe: a failed source
# cannot start or attach to the user's regular JACK/PipeWire instance.
for _attempt in {1..20}; do
  kill -0 "$server_pid"
  "${wrapper[@]}" "$runtime_dir/source" > "$artifact_dir/source.json" \
    2>> "$artifact_dir/source.log" &
  source_pid=$!
  sleep 0.2
  if kill -0 "$source_pid" 2>/dev/null && [[ -s "$artifact_dir/source.json" ]]; then break; fi
  kill "$source_pid" 2>/dev/null || true
  wait "$source_pid" 2>/dev/null || true
  source_pid=
done
[[ -n "$source_pid" && -s "$artifact_dir/source.json" ]]
timeout 10s "${wrapper[@]}" "$build_dir/sync_audio_native_probe" --list \
  > "$artifact_dir/inventory.json" 2> "$artifact_dir/inventory.log"
source_id=$(jq -er '[.sources[] | select(.name == "SyncTest32 (Jack)")] |
  if length == 1 then .[0].id else error("expected exactly one SyncTest32 source") end' \
  "$artifact_dir/inventory.json")
timeout 10s "${wrapper[@]}" "$build_dir/sync_audio_native_probe" \
  --source-id "$source_id" --expect-jack-pattern \
  > "$artifact_dir/capture.json" 2> "$artifact_dir/capture.log"
jq -e '.qualified == true and .channelCount == 32 and .sampleRate == 48000 and
  .jackPattern.validatedFrames >= 48000 and .jackPattern.mismatches == 0' \
  "$artifact_dir/capture.json" >/dev/null
cat "$artifact_dir/capture.json"
