# Bring-up / diagnostics (offline)

A procedure for finding why the printer won't accept data. Do it **in
order**, with a multimeter, no skipping. Each step rules out one cause.

Firmware pin map: **Manawyrm/ESP32_VirtualPrinter** (see `include/config.h`).
Test firmware: `pio run -e bench -t upload` (ignores status, BUSY handshake
only; `@DIAG` / `@PINS` / `@D0..@D7` / `@ST` commands over serial).

---

## 0. Baseline (already confirmed on this build)

- [x] Printer works — self-test prints (holds **LF/FF** while powering on).
- [x] Firmware runs, shows a banner, responds to commands.
- [x] `/STROBE` (GPIO13) has continuity to Centronics pin 1 and **pulses**.
- [x] ESP32 GND has continuity to the Centronics shell.
- [x] A data line driven high ≈ 3.3 V at the terminal.

**And yet the printer does not raise BUSY in response to the strobe.** What's
left to check is below.

---

## 1. Does `/STROBE` go all the way to 0 V?  (most likely cause)

The printer latches the data on the **falling edge** of `/STROBE`. If the
line only swings between 5 V and ~3.3 V (never below 0.8 V), the printer
**never sees the strobe**.

1. Leave **only** `GPIO13 → terminal 1`. **Remove any pull-up / resistor to
   +5 V** on that line.
2. DC multimeter, terminal 1 at rest → should read **~3.3 V** (not 5 V).
   - Reads ~5 V? A wire/bridge ties terminal 1 to +5 V. **Find and remove
     it.** That's the fault.
3. Run `@ST` in a loop and measure terminal 1 → the average should **drop**
   (blinks below 5 V). Better: a scope / logic analyzer should show the
   pulse going to **0 V**.

## 2. Is the shield's DB25 mapped the way you think?

Many breakouts number the terminals out of order, or the silkscreen doesn't
match the connector pin.

- Continuity: **terminal "1"** ↔ **physical pin 1 of the board's DB25**.
- Repeat for terminals 2..9 and 11. If one doesn't match → use the
  connector's number, not the terminal's.

## 3. Is the cable a straight DB25 ↔ Centronics-36?

- Continuity **shield terminal 1** ↔ **Centronics pin 1** (behind the
  printer), with the cable plugged in at both ends.
- Same for terminal 2 ↔ pin 2, terminal 11 ↔ pin 11, terminal GND ↔ pin
  19/16/33.
- Any that don't beep → wrong, badly seated or broken cable. An IEEE-1284
  "A-B" cable: PC end = **male** DB25, printer end = **male** Centronics-36.

## 4. Is `/SELECT-IN` (Centronics 36 / DB25 17) LOW at the printer?

Without it the LX-810 is "deaf" (BUSY stays low, no ACKNLG, ignores the
strobe — technical manual, Table 1-12).

- The firmware already drives `GPIO19 → DB25 17` **LOW**.
- Confirm with the meter: **Centronics pin 36** (at the printer) ≈ **0 V**
  with everything connected.
- If it's high: the GPIO19 → terminal 17 → pin 36 path isn't closing. Ground
  terminal 17 straight to GND as reinforcement.

## 5. Does BUSY come back to the ESP32?

Never confirmed. If the `pin 11 → (1k series or divider) → GPIO17` wire is
open, the firmware reads `BUSY=0` forever and you can't tell whether the
printer responded.

- With `@PINS` running, touch a jumper from **terminal 11 to +5 V** → `BUSY`
  must become **1**.
  - It didn't → the terminal 11 → GPIO17 path is open. Redo it.
- Then **terminal 11 to GND** → back to **0**.

## 6. Status input levels (the ESP32 is not 5 V tolerant)

BUSY/PE/SELECT/ERROR/ACK come from the printer at **5 V**. GPIO 17/16/35/22/15
**cannot** take 5 V directly. Use **~1 kΩ in series** on each (Manawyrm's
recommendation) or a divider. GPIO35 is *input only* — fine for SELECT.

## 7. Fixed ties

- DB25 **14** (/AUTOFEED) → **+5 V** (or the firmware drives GPIO23 high).
- DB25 **17** (/SELECT-IN) → **GND** (or the firmware drives GPIO19 low).
- DB25 **31** (/INIT) → from GPIO21; idle high.

---

## When to ask for help / speed things up

- **A sharp photo of the wiring** (shield + breadboard + ESP32) to review
  against `docs/schematic.svg`.
- A **logic analyzer** on `/STROBE` (13→pin 1) and `BUSY` (pin 11→17): send
  `@ST` and see whether there's a strobe pulse and a BUSY response.
- Someone reviewing **wire by wire** against the table in `config.h`.

## Back to the normal firmware

```
pio run -e esp32dev -t upload
```
