# IdeaPickup changes

Upstream: `78/esp-wifi-connect` 3.3.1, commit recorded in `UPSTREAM_COMMIT`.

- Never erase NVS automatically when initialization fails.
- The configuration AP uses an 8-character WPA2 password, generated once and
  saved in memo_ap/password. Reopening the AP, rebooting and preserving NVS on
  updates keep it unchanged. The physical change-password button persists a
  different password before restarting the AP and replacing its web session.
- Separate Wi-Fi connection/save from current-service configuration. The application
  callbacks expose only direct-cloud credential-presence flags, mode and actual task
  results. The password-protected AP accepts write-only provider credentials;
  responses expose presence flags and generations, never saved values. Replace
  and clear are explicit actions. The bounded USB helper remains available.
  Legacy relay URL/token/pairing handlers have been removed; old NVS stays intact.
- Limit JSON bodies and reject decoded NUL/unsafe structural depth before parsing;
  use the narrow DONOR request guard (fixed AP Host/Origin, fresh session CSRF).
  All existing mutation handlers require this guard; hidden upstream advanced and
  saved-network endpoints are not exposed as extra product controls.
- Escape arbitrary SSIDs in scan JSON, report connection/save failure, and keep
  the temporary hotspot open until explicit completion so status can be checked.
- Allow 8 KiB HTTP task stack for synchronous bounded current-service persistence.
- Show scanned networks with signal and lock icons in the selected design's
  picker; retain manual SSID entry. Mobile browsers do not consistently expose
  an HTML datalist as a tappable network list.
- Own one default AP and one default STA esp-netif for the Wi-Fi driver lifetime.
  ESP-IDF default AP_START/AP_STOP handlers start/stop DHCP; the portal waits
  for these events before serving or returning from Stop. No manual DHCP restart.
  Wi-Fi save still waits for the actual STA DHCP address, not association.
- Never print a received Wi-Fi password to the serial log.
- Before switching networks, drain the old station disconnect event; cancel
  failed or timed-out attempts before retrying. Treat station configuration
  errors as connection failures rather than fatal assertions, preserving the
  portal and previously saved Wi-Fi when a new connection fails.

The upstream MIT license is retained in `LICENSE`.

## R12 provisioning rebuild references

- ESP-IDF v5.5.1, pinned commit `fcae32885b0296b32044cb99ecbdc50d98dddb83`:
  [`examples/wifi/softap_sta/main/softap_sta.c`](https://github.com/espressif/esp-idf/blob/fcae32885b0296b32044cb99ecbdc50d98dddb83/examples/wifi/softap_sta/main/softap_sta.c)
  for default AP/STA interfaces and SDK DHCP ownership;
  HTTP LRU session eviction is retained from the official HTTP server practice.
  No SDK or dependency version is modified.
- Bound HTTPD to four client sessions and retain SDK LRU eviction. Its three
  internal sockets plus the locked lwIP limit of ten leave three descriptors
  for the device's outbound HTTP/TLS workers; the SDK default of seven clients
  alone consumes the whole pool before a competing worker opens a connection.
- The pinned Xiaozhi author's component supplies manager serialization, APSTA
  connection retries and saved SSIDs. Project service/CSRF, bounded request handling,
  separate Wi-Fi save and explicit exit remain. Unused SmartConfig and upstream
  advanced/saved-network routes are removed from this project's portal.
- Scanning uses the SDK scan API when the existing page loads or Refresh is tapped.
  No idle periodic scan timer; no new user step or setting.
- 2026-10-14: manual browser configuration replaces captive auto-login. Removed
  wildcard DNS source/header, DNS task/socket, 302 fallback, unused done page,
  embedded asset entries and the obsolete parser-only test. Unknown HTTP paths
  return ordinary 404; no synthetic Internet-success response is sent.
- Reuse the existing application network worker for bounded Wi-Fi connect/save;
  the HTTP task only accepts a request. AP and STA stay up after a successful
  save so users can inspect actual connection and direct-cloud task status.
  AP Stop wakes and joins an in-flight connection before driver teardown. Saved
  Wi-Fi reconnects when settings reopen. Raw passwords/tokens are never status fields.
- HTTP accepted sockets must target the AP interface address; the simultaneous
  STA connection does not expose device configuration on the home LAN. Accepted
  sockets are also bound to the AP netif.
- Phone reconnection, manual browser, exit/re-entry, ordinary update and audio/USB
  integration require current real-device evidence. These changes do not assert the
  cause of the prior intermittent failure.

- The provisioning page has an offline, self-contained visual/interaction design
  supplied through Gemini Canvas. Integration keeps its styling intact, uses the
  existing relative API routes, protects edits from overlapping responses, and
  separates accepted settings from actual Wi-Fi/cloud-task results; saving credentials is not provider verification. The product
  title is IdeaPickup across both supported hardware variants. No auto-popup,
  new endpoint, persistent browser storage, or third-party asset is added.

## Direct-cloud cleanup

Unused language configuration was removed: this project embeds one Chinese
page and does not select page assets by language. Historical relay state is
read only by compatibility decoders; current credentials and mode use the
independent direct-cloud configuration.
