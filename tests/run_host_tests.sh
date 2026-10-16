#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$project_dir/private/host-tests"
chmod 700 "$project_dir/private/host-tests"
test_dir="$(mktemp -d "$project_dir/private/host-tests/run.XXXXXX")"
trap 'rm -rf -- "$test_dir"' EXIT
mkdir -p "$test_dir/fixtures"

"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
  -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
  -I "$project_dir/main" "$project_dir/main/memo_store.c" \
  "$project_dir/tests/test_store.c" -o "$test_dir/test_store"
"$test_dir/test_store" "$test_dir/fixtures"
"${PYTHON:-python3}" "$project_dir/tests/check_wavs.py" \
  "$test_dir/fixtures/ten" --min-count 10 --min-duration 120

# State logic is maintained by the application owner and joins this suite once
# present. It must remain portable and link without ESP-IDF or hardware stubs.
if [[ -f "$project_dir/tests/test_state.c" && -f "$project_dir/main/state.c" ]]; then
  "${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
    -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
    -I "$project_dir/main" "$project_dir/main/state.c" \
    "$project_dir/tests/test_state.c" -o "$test_dir/test_state"
  "$test_dir/test_state"
fi

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
  -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
  -I "$project_dir/main" "$project_dir/main/power_state.c" \
  "$project_dir/tests/test_power_state.c" -o "$test_dir/test_power_state"
"$test_dir/test_power_state"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
  -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
  -DCONFIG_MEMO_VOCAT16=1 -I "$project_dir/main" "$project_dir/main/power_state.c" \
  "$project_dir/tests/test_vocat16_power.c" -o "$test_dir/test_vocat16_power"
"$test_dir/test_vocat16_power"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
  -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
  -DCONFIG_MEMO_VOCAT16=1 -I "$project_dir/main" \
  "$project_dir/main/memo_gauge_profile.c" "$project_dir/main/power_state.c" \
  "$project_dir/tests/test_gauge_profile.c" -o "$test_dir/test_gauge_profile"
"$test_dir/test_gauge_profile"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
  -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
  -I "$project_dir/main" "$project_dir/main/power_source.c" \
  "$project_dir/tests/test_power_source.c" -o "$test_dir/test_power_source"
"$test_dir/test_power_source"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
  -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
  -I "$project_dir/main" "$project_dir/main/network_sync_state.c" \
  "$project_dir/tests/test_network_sync_state.c" -o "$test_dir/test_network_sync_state"
"$test_dir/test_network_sync_state"

# Serial parser and timeout tests use FakeSerial only, never the connected board.
"$project_dir/.venv/bin/python" "$project_dir/tests/test_device.py"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 \
  -fsanitize=address,undefined -I "$project_dir/main" "$project_dir/main/voice_gate.c" \
  "$project_dir/tests/test_voice_gate.c" -o "$test_dir/test_voice_gate"
"$test_dir/test_voice_gate"

"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -I "$project_dir/main" "$project_dir/main/memo_sync_cache.c" \
  "$project_dir/tests/test_sync_cache.c" -o "$test_dir/test_sync_cache"
"$test_dir/test_sync_cache" "$test_dir/sync.bin"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 \
  -fsanitize=address,undefined -I "$project_dir/main" "$project_dir/main/memo_cardless_buffer.c" \
  "$project_dir/tests/test_cardless_buffer.c" -o "$test_dir/test_cardless_buffer"
"$test_dir/test_cardless_buffer"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 \
  -fsanitize=address,undefined -I "$project_dir/main" "$project_dir/main/memo_cardless_state.c" \
  "$project_dir/tests/test_cardless_state.c" -o "$test_dir/test_cardless_state"
"$test_dir/test_cardless_state"
"${PYTHON:-python3}" "$project_dir/tests/test_usb_audio_check.py"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 \
  -fsanitize=address,undefined -I "$project_dir/main" "$project_dir/main/memo_usb_pcm.c" \
  "$project_dir/tests/test_usb_pcm.c" -o "$test_dir/test_usb_pcm"
"$test_dir/test_usb_pcm"

# Production SD polling / idle handoff with synthetic hardware and power state.
"${PYTHON:-python3}" "$project_dir/tests/test_sd_probe.py"
