# Protocol and usage — serial → parallel bridge

## Physical layer (PC side)

- **ESP32 UART0 over USB** (the same channel used to flash the firmware).
- 8N1, no hardware flow control.
- Baud: `SERIAL_BAUD` (default **115200**). You can raise it (e.g. 921600) in
  `platformio.ini`, but the printer is the bottleneck — the LX-810L does a
  few hundred characters/s with a small buffer.
- **XON/XOFF** (optional, on by default): when the internal queue fills, the
  ESP32 sends `XOFF` (0x13) to the host; when it drains, `XON` (0x11). Keeps
  you from dropping bytes when dumping big files.

## Data flow

Everything that arrives on Serial is sent to the printer **byte by byte**,
with the full Centronics handshake (wait for `BUSY` low → drive D0–D7 →
pulse `/STROBE`). Before each byte the firmware also checks `SELECT`
(on-line), `PE` (paper) and `/ERROR`.

### TEXT MODE (default, `-D PRN_TEXT_MODE=1`)

`CR`, `LF` and `CRLF` are all normalized to **`CRLF`**. This is what you want
for text typed into a terminal (otherwise the LX-810L staircases or
overprints the line).

### RAW MODE (`-D PRN_TEXT_MODE=0`)

The stream passes **through untouched**. Use it for binary ESC/P, bit-image
graphics, barcodes, etc. Best combined with `-D PRN_NO_BANNER` and an
`ESC @` at the start of the job.

> Note: at power-up the **ESP32 ROM bootloader** always prints one line
> (`rst:0x1...`) on UART0 at 115200. That's unavoidable on a UART0 bridge.
> In RAW mode, start the job with `ESC @` so the printer ignores any leftover.

---

## How to send (macOS / Linux)

Find the port:

```bash
pio device list
# macOS:  /dev/cu.usbserial-XXXX  or  /dev/cu.wchusbserialXXXX  or  /dev/cu.SLAB_USBtoUART
# Linux:  /dev/ttyUSB0  or  /dev/ttyACM0
```

### 1) Serial monitor (type and print)

```bash
pio device monitor
# wait for the "PRONTO." line, then type; each Enter prints the line
```

### 2) A quick one-liner

```bash
# macOS
printf 'Hello world\r\n\f' > /dev/cu.usbserial-XXXX
# Linux
printf 'Hello world\r\n\f' > /dev/ttyUSB0
```

`\f` (0x0C, form feed) ejects / advances the page.

If the OS mangles the port, pin the parameters first:

```bash
# macOS
stty -f /dev/cu.usbserial-XXXX 115200 raw -echo
# Linux (note -F)
stty -F /dev/ttyUSB0 115200 raw -echo ixoff
```

### 3) A whole file

```bash
cat report.txt > /dev/cu.usbserial-XXXX
```

### 4) Python (pyserial)

```python
import serial, time
p = serial.Serial("/dev/cu.usbserial-XXXX", 115200)
time.sleep(2)                      # wait for the ESP32 to reboot
p.write("ESP32 + LX-810L\r\n".encode("cp850"))
p.write(b"\x0C")                   # form feed
p.close()
```

### 5) Over the network (HTTP) — `WEBPRINT_ENABLE` (default)

The ESP32 joins WiFi as a station (`WEBPRINT_WIFI_SSID` / `_PASS` in
`include/config.h`, the relay's by default) and serves HTTP on port 80. The
IP shows on the serial monitor: `[web] ready -> http://192.168.x.y/`. You
can also use `http://dotmatrix.local/` (mDNS, name in `WEBPRINT_HOSTNAME`).

| Route | Method | What it does |
|-------|--------|--------------|
| `/` | GET | HTML page: `<textarea>` + **Print** button + status badge |
| `/print` | POST | enqueue text: form field `texto` **or** the raw body with `Content-Type: text/plain` |
| `/status` | GET | JSON: `pronta`, `estado`, `fila_livre`, `fila_total`, `fila_vazia`, `ip` |

```bash
# raw body (needs Content-Type: text/plain)
curl -sS --data-binary $'Report\r\n\f' -H 'Content-Type: text/plain' \
     http://dotmatrix.local/print
# a whole file
curl -sS --data-binary @report.txt -H 'Content-Type: text/plain' \
     http://dotmatrix.local/print
# via the form field
curl -sS --data-urlencode 'texto=Hello world' http://dotmatrix.local/print
# status
curl -sS http://dotmatrix.local/status
```

Without the `Content-Type: text/plain` header, `curl --data*` sends
`application/x-www-form-urlencoded`, so you must use the `texto` field
(`--data-urlencode 'texto=...'`). Received text goes into the **same queue**
as the serial bridge and passes through TEXT MODE (CR/LF → CRLF). Limits:
`WEBPRINT_MAX_BODY` (16 KB) per request; `WEBPRINT_FEED_TIMEOUT_MS` (20 s)
for the queue to drain. Optional token: `WEBPRINT_TOKEN` != `""` requires
`?token=...` or the `X-Auth-Token` header on `/print` and `/status`. Turn it
all off with `-D WEBPRINT_ENABLE=0`.

### 6) As a network printer (Cmd+P) — `RAWPRINT_ENABLE` (default)

Raw TCP on port **9100** (JetDirect), advertised over mDNS as
`EPSON LX-810L`. Add it by IP: *HP Jetdirect – Socket*, address
`dotmatrix.local`, and pick a driver:

- **Generic Text Only** — plain text, no formatting. Reliable.
- **9-pin ESC/P** (CUPS "Epson 9-Pin Series", `drv:///sample.drv/epson9.ppd`)
  — text + bitmap graphics as ESC/P, executed by the LX-810L. Requires
  `-D RAWPRINT_RAW=1` (pure passthrough).

It is **not AirPrint** — an ESP32 can't render PDF / PWG-raster.

---

## ESC/P — quick reference (LX-810L, 9-pin)

| Bytes            | Effect |
|------------------|--------|
| `1B 40` (`ESC @`)| reset the interpreter |
| `0C` (`FF`)      | form feed |
| `0A` (`LF`)      | line feed |
| `0D` (`CR`)      | carriage return |
| `1B 45` / `1B 46`| bold on / off |
| `1B 34` / `1B 35`| italic on / off |
| `1B 2D 01` / `1B 2D 00` | underline on / off |
| `1B 78 01` / `1B 78 00` | NLQ / draft quality |
| `1B 30` / `1B 32`| line spacing 1/8" / 1/6" |
| `1B 43 n`        | page length = n lines |
| `1B 43 00 n`     | page length = n inches |
| `0F` (`SI`) / `12` (`DC2`) | condensed on / off |
| `0E` (`SO`) / `14` (`DC4`) | expanded (double width) for this line only |
| `1B 52 n`        | national character set |
| `1B 74 n`        | select character table |
| `1B 36`          | make 80h–9Fh printable |

### Accents / non-ASCII

The LX-810L uses **single-byte tables** (PC437, **PC850**, **PC860
Portugal**, …). A modern terminal sends **UTF-8** (multibyte) → accents come
out wrong.

Fixes:
1. Set the table on the printer (DIP switches or `ESC t n` / `ESC R n`) and
   send text already in **CP850** or **CP860** (like the Python example
   above).
2. Automatic UTF-8 → CP850 conversion inside the firmware is on the
   **roadmap** (`README.md`).

---

## Troubleshooting

| Symptom | Likely cause | What to do |
|---------|--------------|------------|
| Nothing prints, **LED blinking** | offline / out of paper / error; `SELECT` low | power the printer, load paper, press *On Line*; check the dividers and **common GND** |
| Nothing prints, **no error** | `/SELECT-IN` (DB25-17) not at GND; or an auto-select switch | ground DB25-17; or set the DIP |
| Output "**staircases**" (no CR) | RAW mode and the host only sends `\n` | use TEXT mode, or send `\r\n` |
| **Double spacing** | `/AUTOFEED` active + we already send CRLF | move DB25-14 off GND / tie it to **+5V** |
| **Garbled characters** | data level or timing; TXS0108E unstable | check `VCCB=5V` and GND; **swap for 74HCT541/245**; lower the baud |
| Only prints **after a lot of text** | printer buffer (normal) | send `FF` or `\r\n` at the end of the job |
| **Drops characters** on a big file | no host flow control | keep `PRN_XONXOFF=1` **and** `stty ... ixoff`, or lower the baud |
| ESP32 **reboots** when opening the monitor / flashing | normal (USB-serial DTR/RTS pulse) | wait for the `PRONTO.` banner |
| Wrong accents | UTF-8 vs single-byte table | see "Accents" above |
| `dotmatrix.local` **doesn't resolve** | mDNS blocked, or OS without Bonjour/avahi | use the IP directly (serial monitor); on Linux install `avahi-daemon` |
| `POST /print` → **503** | printer offline / no paper / error | same checklist as "LED blinking" |
| `POST /print` → **504** | the queue didn't drain (slow or stuck printer) | check the printer; resend; raise `WEBPRINT_FEED_TIMEOUT_MS` |
| Page opens but **"estado: —"** | `/status` blocked by a token, or JS disabled | check `WEBPRINT_TOKEN`; printing via the form works without JS |
| Web won't connect / no IP in the log | wrong SSID/password in `WEBPRINT_WIFI_*` | fix it in `include/config.h`; the log shows `WiFi sem associacao` |
