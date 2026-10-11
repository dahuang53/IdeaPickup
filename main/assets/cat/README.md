# ESP-Brookesia speaker emotion assets

The `.aaf` files in this directory are unmodified selected animation assets from
Espressif's ESP-Brookesia `release/v0.6`, commit
`b22c488f50bafe53342c8e171081bd736396ef58`:

https://github.com/espressif/esp-brookesia/tree/b22c488f50bafe53342c8e171081bd736396ef58/core/brookesia_core/systems/speaker/assets/animations/emotion

They are used under the Apache License 2.0 reproduced in `LICENSE`.

`emotion_blink_cardless_284_126.aaf` is a project-derived variant of the
unchanged `emotion_blink_slow_284_126.aaf`. Run
`python3 scripts/generate_cardless_blink.py` from the project root to recreate
it. The derivation retains the same compressed eye images, removes five
consecutive repeated frames and the 99-frame static tail, then updates the AAF
header/checksum. It is used only for the faster no-card blink.
