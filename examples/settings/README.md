# Settings

The board's settings screen, made with LVGL MPY Studio and its MCP tools. For now it sets up Wi-Fi:
scan, choose a network, type the password on the touch keyboard, save and connect.

The choice is saved in `settings.json` in the board's folder (`/usr/local/mpy/fs` on the
STM32MP135F-DK), which every app reads through the frozen modules of the firmware:

```python
import mpy_settings, mpy_wifi
mpy_settings.get("wifi")     # {"ssid": ..., "password": ...}
mpy_wifi.ensure()            # connect to that network unless already on it
mpy_wifi.online()            # True with a default route (Wi-Fi or cable)
```

The web radio calls `mpy_wifi.ensure()` at start, so once the network is set here the radio finds
it by itself. Scan and connect run in the background; the page's timer follows them.

`mpy_wifi` writes the standard system configuration: `/etc/wpa_supplicant/wpa_supplicant-wlan0.conf`
(with the hashed key from `wpa_passphrase`, the network name in hex) for `wpa_supplicant@wlan0`, and
`/etc/systemd/network/25-mpy-wifi.network` for DHCP. `mpy-uninstall` removes them.

The password is kept in clear in `settings.json`: anyone who can read the board's files can read it.

In the Studio simulator there is no Wi-Fi: three demo networks are shown and the settings stay in
memory.
