#!/usr/bin/env bash
# Small image/Compose/broker subset. Full deployment acceptance belongs to R09.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ "$#" != 2 || "$1" != --platform ]]; then
  printf '%s\n' 'usage: test_container_ci.sh --platform linux/amd64|linux/arm64' >&2
  exit 2
fi
case "$2" in linux/amd64|linux/arm64) memo_ci_platform="$2";; *) exit 2;; esac
umask 077
mkdir -p private/container-ci
memo_ci_dir="$(mktemp -d "$PWD/private/container-ci/run.XXXXXX")"
# No tags, push, deployed Compose stack, host secrets or Docker socket mounts.
docker build --platform "$memo_ci_platform" --target runtime \
  -f server-relay/Dockerfile --iidfile "$memo_ci_dir/runtime.id" .
memo_ci_image="$(cat "$memo_ci_dir/runtime.id")"
docker image inspect "$memo_ci_image" > "$memo_ci_dir/runtime-inspect.json"
python3 - "$memo_ci_dir/runtime-inspect.json" "$memo_ci_platform" <<'PY'
import json,sys
item=json.load(open(sys.argv[1]))[0]
assert item['Os']+'/'+item['Architecture']==sys.argv[2]
assert item['Config']['User']=='10001:10001'
assert item['Config']['Entrypoint']==['python','-m','relay']
PY
docker run --rm --platform "$memo_ci_platform" --network none --read-only \
  --cap-drop ALL --security-opt no-new-privileges:true \
  --entrypoint python "$memo_ci_image" -c \
  'import os; from stream_service import OpusLibrary; assert os.getuid()==10001; OpusLibrary()'
VOICE_MEMO_IMAGE="$memo_ci_image" VOICE_MEMO_PUBLIC_URL=https://relay.example.invalid/memo \
  docker compose -p voice-memo-ci -f deploy/compose.yaml config --quiet
VOICE_MEMO_IMAGE="$memo_ci_image" VOICE_MEMO_PUBLIC_URL=https://relay.example.invalid/memo \
  VOICE_MEMO_TLS_DIR="$memo_ci_dir" docker compose -p voice-memo-ci \
  -f deploy/compose.yaml -f deploy/compose.proxy.yaml config --quiet
docker build --platform "$memo_ci_platform" --target verification \
  -f server-relay/Dockerfile --iidfile "$memo_ci_dir/verification.id" .
docker run --rm --platform "$memo_ci_platform" --network none --read-only \
  --cap-drop ALL --security-opt no-new-privileges:true \
  --tmpfs /var/lib/voice-memo-work:rw,noexec,nosuid,size=16m,mode=0700 \
  "$(cat "$memo_ci_dir/verification.id")"
printf '%s\n' "PASS container CI subset: $memo_ci_platform; records: $memo_ci_dir"
