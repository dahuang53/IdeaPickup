# VoiceMemo changes

Upstream: `78/esp-wifi-connect` 3.3.1, commit recorded in `UPSTREAM_COMMIT`.

- Never erase NVS automatically when initialization fails.
- Allow the temporary captive portal AP to use a caller-supplied WPA2 password.
- Separate Wi-Fi connection/save from current-service configuration. The application
  callbacks expose only a redacted view and save the existing R03 settings; pairing
  runs/replays on the ordinary worker, with no new phase or history operation.
- Limit JSON bodies and reject decoded NUL/unsafe structural depth before parsing;
  use the narrow DONOR request guard (fixed AP Host/Origin, fresh session CSRF).
  All existing mutation handlers require this guard; hidden upstream advanced and
  saved-network endpoints are not exposed as extra product controls.
- Escape arbitrary SSIDs in scan JSON, report connection/save failure, and keep
  the protected hotspot open until explicit completion so status can be checked.
- Allow 8 KiB HTTP task stack for synchronous bounded current-service persistence.
- Show scanned networks in an always-visible select control and copy the chosen
  SSID into the editable field; mobile browsers do not consistently expose an
  HTML datalist as a tappable network list.
- Create the STA esp-netif while the portal runs in APSTA mode and wait for its
  DHCP address before acknowledging the independent Wi-Fi save.
- Never print a received Wi-Fi password to the serial log.

The upstream MIT license is retained in `LICENSE`.
