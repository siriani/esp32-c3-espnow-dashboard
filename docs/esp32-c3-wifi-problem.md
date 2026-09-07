# The ESP32-C3 WiFi problem

This is the reason the project is split in two. If you have a cheap
ESP32-C3 + ST7735 "mini TV / desktop trinket" module and its WiFi refuses to
connect to your router, this page is for you.

## Symptom

- Station never reaches `WL_CONNECTED`.
- The disconnect event fires immediately and repeatedly with
  `reason = 2` (`WIFI_REASON_AUTH_EXPIRE`); sometimes `15` / `205`.
- `WiFi.status()` sits at `WL_DISCONNECTED` / `WL_NO_SSID_AVAIL`.
- The target SSID **is** in the scan results, strong (-24 dBm here),
  `WIFI_AUTH_WPA2_PSK`.
- The **same board connects fine to a phone hotspot** with a short password.
- Reproduced on **two** home routers of different brands.
- The password is correct (other devices use it).

`reason 2` is the AP deauthenticating the station: it received the auth /
association but dropped it. With a correct PSK, a strong signal and no PMF,
that points at RF quality during the 4-way handshake, not at credentials.

## What did not work

All of this was tried, individually and in combination, and none of it made
the association succeed:

| Attempt | Result |
|---|---|
| `WiFi.setTxPower()` at 2, 5, 8.5, 10, 15, 19.5 dBm | `reason 2` |
| Arduino-ESP32 core **2.0.17** (IDF 4.4) and **3.3.x** (IDF 5.x) | `reason 2` on both |
| `esptool erase_flash` (wipe NVS / PHY cal) + reflash | `reason 2` |
| `WiFi.begin(ssid, pass, channel, bssid)` — lock BSSID + channel | `reason 2` |
| `WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN)` + sort by RSSI | `reason 2` |
| `esp_wifi_set_country()` with an open 1–13 policy | `reason 2` |
| `esp_wifi_set_ps(WIFI_PS_NONE)` | `reason 2` |
| `esp_wifi_set_protocol()` to 11b/g and to 11b/g/n | `reason 2` |
| `esp_wifi_set_bandwidth(WIFI_BW_HT20)` | `reason 2` |
| `WiFi.setMinSecurity(WIFI_AUTH_WEP / WIFI_AUTH_OPEN)` | `reason 2` |
| `pmf_cfg.required = true` via `esp_wifi_set_config()` | `reason 201` (no AP found — so the APs are **not** PMF-required) |
| Disable the Bluetooth controller | `reason 2` (BT wasn't up anyway) |
| "scan then `setTxPower(40)` then `begin()`" forum recipe | `reason 2` |
| ~3 minutes of continuous retries | never associates (`assocOK == 0`) |
| Router: fixed channel 1/6/11, 20 MHz only, WPA2-AES only, WMM / beamforming / band-steering off | `reason 2` |

Phone hotspot: connects every time.

## Root cause

The module is an **ESP32-C3 bare chip** (not a pre-certified module), so the
2.4 GHz matching network is on the board. On this board's schematic two
components in that network are marked `TBD` / `DNP` (not fitted). The radio
transmits and receives — it sees every AP, and it connects to forgiving ones
— but the RF is marginal enough that the WPA2 4-way handshake, which is
timing sensitive, fails against "normal" access points.

This is the same failure mode widely reported for the "ESP32-C3 SuperMini"
class of boards. On boards where an antenna rework is possible people solder
a ~31 mm quarter-wave wire to the feed point and it starts working. On a
sealed module with an internal trace antenna there is nothing to solder.

## The fix: ESP-NOW instead of WiFi

ESP-NOW does not have any of the parts that were failing:

- no association, no 4-way handshake, no DHCP;
- connectionless single **action frames**;
- robust low-rate GFSK modulation with FEC;
- broadcast is fine, so no pairing and any number of listeners.

On the exact same marginal radio, ESP-NOW at in-house range is reliable.

So:

- A **second ESP32** (a WROOM-32 here — anything with a real antenna works)
  keeps a normal WiFi connection and does HTTPS, MQTT and NTP.
- It sends the C3 small typed packets (`enow_prices_t`, `enow_weather_t`,
  `enow_mqtt_t` — see [`../shared/enow_proto.h`](../shared/enow_proto.h)),
  broadcast, once every 60 s for prices / 10 min for weather / on arrival
  for MQTT.
- The C3 runs **no WiFi**. It does
  `WiFi.mode(WIFI_STA); WiFi.disconnect();` only to bring the PHY up, then
  `esp_now_init()` and `esp_wifi_set_channel()`.

### Sharing a channel with a roaming router

ESP-NOW sender and receiver must be on the same channel. The relay is a
station, so its channel = the router's, and consumer routers move channels.
Handled like this:

1. Relay puts `WiFi.channel()` in every price packet (`relay_ch`).
2. On boot the C3 sweeps a small candidate list (`{6, 10, 1, 11}` here),
   ~4 s each, until it receives its first **price** packet.
3. From then on it just follows `relay_ch`.
4. If prices stop arriving for ~2.5 min it unlocks and sweeps again.

No coordination, no config, survives the router hopping channels.

## If your C3 works fine on WiFi

Then you don't need the relay at all for the data side — point the display
firmware straight at your APIs. The relay is still useful as the printer
bridge. But if you're reading this page, your C3 probably doesn't.
