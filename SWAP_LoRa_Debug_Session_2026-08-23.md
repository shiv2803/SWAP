# SWAP — LoRa Bring-Up Debug Session

**Date:** 2026-08-23
**Scope:** Roadmap Item 1 (two-way LoRa ack) → hardware fault investigation
**Boards:** ESP32 DevKit V1 ×2 + Waveshare Core1262-HF (SX1262) ×2
**Outcome:** Firmware work complete. Blocked on a physical wiring fault on the MOSI line. Radio hardware confirmed healthy.

---

## 1. Executive Summary

Roadmap Item 1 was implemented as specified. During hardware testing, the LoRa radio began failing with RadioLib error `-2` (`CHIP_NOT_FOUND`). Several firmware-level hardening fixes were made along the way (all legitimate, all retained), but the root cause turned out to be **electrical, not software**.

A purpose-built raw-SPI diagnostic (`lora_spi_diag`, no RadioLib) established that:

- The SX1262 module **is powered, grounded, and fully alive**
- **MISO, SCK and NSS are all working** — the chip answers with valid status
- **MOSI (GPIO23) is not delivering commands** — every command is rejected

The next action is physical: replace the GPIO23↔module-MOSI jumper, or remap MOSI to a free GPIO if the pad is damaged.

---

## 2. Timeline of the Investigation

| # | Symptom observed | Hypothesis | Verdict |
|---|---|---|---|
| 1 | `-2` after ~25 exchanges | Radio wedged mid-RX after failed receive | Partly right — real bug, not root cause |
| 2 | `-2` after ~5 exchanges | Radio needs full reinit on repeated faults | Partly right — real bug, not root cause |
| 3 | `-2` at `begin()` itself | Physical/electrical fault | Correct direction |
| 4 | BUSY reads LOW | Not a stuck-busy chip | Ruled out |
| 5 | `-2` + BUSY HIGH after rewire | NSS/pin conflict | **Wrong** — later disproven |
| 6 | Raw SPI: BUSY falls in 2 ms | Chip is alive | Confirmed |
| 7 | Raw SPI: all bytes `0x2A` | "Floating MISO / dead bus" | **Wrong** — misread |
| 8 | `0x2A` decoded = `STDBY_RC` + `CMD_FAILED` | Chip is answering; MOSI broken | **Correct** |
| 9 | Swapped-pin + slow-clock retries all fail | MOSI wire genuinely open | Current conclusion |

---

## 3. Roadmap Item 1 — Two-Way LoRa Ack (COMPLETE)

Implemented exactly per the TAG framework spec.

### `lora_bringup_a.ino` (Node A)
- After each `sendLoRaTelemetry()`, calls `receiveLoRaTelemetry()` with a 1.5 s timeout
- Validates the reply matches `PONG <seq>` for the sequence just sent
- Computes true round-trip time (send start → ack received)
- Logs `rtt=<n>ms` per exchange

### `lora_bringup_b.ino` (Node B)
- After a successful receive, parses the seq from `PING <seq>`
- Echoes `PONG <seq>` back via `sendLoRaTelemetry()`
- Logs local reply latency

> **Design note:** Node B cannot measure a true cross-node round trip — it never observes its own PONG's flight time back to A. It therefore reports *local reply latency* (PING parsed → PONG handed to radio), labelled honestly rather than presented as an RTT.

---

## 4. Firmware Fixes Applied (all retained)

These were found by auditing `lora_link.cpp` and `config.h`. All are genuine defects, independent of the wiring fault.

### 4.1 Radio never recovered to standby on receive failure
`receiveLoRaTelemetry()` had three failure exits (timeout, bad packet length, read error) and **none** called `standby()`. Per [RadioLib #575](https://github.com/jgromes/RadioLib/issues/575), this leaves the SX1262 wedged, after which *every* subsequent call — including `transmit()` — returns `-2`.

**Fix:** `lora.standby()` added to all three failure paths.

### 4.2 Duty-cycle tracker silently undersized
`DUTY_MAX_RECORDS` was 256. At ~1 exchange/second (up to 2 transmissions each), it filled in under 5 minutes. Once full, `dutyRecordTransmission()` silently stopped recording — so `dutyCanTransmit()` kept approving transmissions that never counted against the budget.

**Impact:** a silent IN865 1% duty-cycle violation with no error message.
**Fix:** raised to 4096.

### 4.3 TCXO voltage and preamble length left implicit
The hardware reference states the Core1262-HF TCXO is always 1.6 V and warns against assuming defaults — but neither value was passed to `lora.begin()`.

**Fix:** added `LORA_TCXO_VOLTAGE 1.6f` and `LORA_PREAMBLE_LEN 8` to `config.h`, passed explicitly.

### 4.4 No recovery path for a truly stuck radio
`standby()` itself talks over SPI, so it cannot recover a chip that is genuinely stuck.

**Fix:** consecutive-failure counter triggering a full `begin()` reinit (re-toggles `LORA_RST`) after 5 consecutive **real** radio faults. Normal ack timeouts explicitly do *not* count, so it never fires spuriously.

### 4.5 OLED removed entirely (user request)
- `.ino` files: dropped `oled_display.h` include, `setupOled()`, all `showStatus()` calls — status now Serial-only
- `config.h`: removed all `OLED_*` pin defines
- `oled_display.cpp/h` moved to `_to_delete/` in both folders
- Removes the `U8g2lib`/`Wire` dependency

---

## 5. The Diagnostic Tooling Built

### `lora_spi_diag/lora_spi_diag.ino`
Raw SPI probe with **no RadioLib**, because `-2` and `-705` only mean "wrong answer" — they cannot distinguish an unpowered module from a broken wire from a dead chip.

Final version (v5) tests:

| Step | What it does |
|---|---|
| 0 | Hardware reset; times how long BUSY takes to fall |
| A | Normal pins @ 2 MHz |
| B | **MOSI/MISO swapped** @ 2 MHz |
| C | Normal pins @ 500 kHz |
| D | Swapped pins @ 500 kHz |

Each configuration runs `GetStatus`, reads the version-string register, then sends `Calibrate(0x89)` and watches **BUSY** — an independent return path that proves command receipt without depending on anything read back over SPI.

### `pin_integrity/pin_integrity.ino`
Tests GPIO23 four ways (pull-up, pull-down, driven high, driven low) against GPIO18/21 as controls, to separate:
- broken wire / cold joint → pad behaves normally
- damaged or shorted pad → `CANNOT DRIVE HIGH` or `HELD LOW`

### `hello_serial/hello_serial.ino`
Minimal serial sanity check to isolate board/upload/monitor problems from LoRa code.

---

## 6. The Decisive Evidence

Final `lora_spi_diag` v5 output:

```
[0] Power/reset check: BUSY dropped -- module powered, grounded, RST+BUSY wired OK.
  [A] normal   SCK=18 MISO=19 MOSI=23 @ 2 MHz
      status 0x2A (mode=STDBY_RC, cmd=CMD_FAILED)  version="****"  BUSY: no reaction
  [B] SWAPPED  SCK=18 MISO=23 MOSI=19 @ 2 MHz
      status 0x00 (mode=INVALID, cmd=ok/none)      version="...."  BUSY: no reaction
  [C] normal   @ 500 kHz   -> identical to [A]
  [D] SWAPPED  @ 500 kHz   -> identical to [B]
```

### Decoding `0x2A`

```
0x2A = 0b0010_1010
  bits 6:4 = 0x20 -> STDBY_RC    (correct post-reset chip mode)
  bits 3:1 = 0x0A -> CMD_FAILED  ("SPI command failed to execute")
```

**This is a valid status byte, not noise.** A floating line cannot produce a value that decodes to precisely the expected chip mode.

### What each result proves

| Observation | Conclusion |
|---|---|
| BUSY falls 2 ms after RST | Power, GND, RST, BUSY wires all good |
| Status decodes to `STDBY_RC` | **MISO, SCK, NSS all working** |
| Config A returns real data, B returns `0x00` | MISO is correctly on GPIO19, **not swapped** |
| `cmd=CMD_FAILED` | Chip receives garbage it cannot execute |
| Calibrate never moves BUSY | Commands are not arriving |
| Slow clock (C/D) doesn't help | Not a signal-integrity/marginal-timing issue |

**Mechanism:** an open MOSI line clocks in all-1s (`0xFF`), which is not a valid opcode, so every command fails — while the SX126x still shifts its status byte out on MISO. Hence the same byte on every read.

---

## 7. Current Verdict

> **MOSI (GPIO23) → module MOSI is not conducting.**
> The SX1262 itself is healthy. This is a single broken connection.

### Next actions

1. **Flash `pin_integrity`** (module connected) to determine which:
   - **GPIO23 behaves like GPIO18/21** → pad healthy; the wire or the module's MOSI header joint is open. Replace the jumper outright (a wire can break inside intact-looking insulation), then reflow the header pin.
   - **GPIO23 shows `CANNOT DRIVE HIGH` / `HELD LOW`** → pad damaged or net shorted. **Remap MOSI** to a free GPIO (GPIO4 or GPIO13, both free since the OLED was removed):
     ```cpp
     SPI.begin(18, 19, 4, 21);   // SCK, MISO, MOSI→GPIO4, NSS
     ```
     The ESP32 GPIO matrix routes SPI to arbitrary pins with no penalty — this is a real fix, not a workaround.

2. Once the bus passes (`BUSY: REACTED`), reflash the bring-up sketches. Item 1 should then work as written.

---

## 8. Pin Map (final, as reverted)

| Signal | GPIO | Notes |
|---|---|---|
| SPI SCK | 18 | RadioLib VSPI default |
| SPI MISO | 19 | ✅ verified working |
| SPI MOSI | 23 | ❌ **suspected open** |
| NSS (CS) | 21 | Reverted from GPIO13 experiment |
| RST | 33 | ✅ verified working |
| BUSY | 27 | ✅ verified working |
| RXEN | 25 | |
| TXEN | 32 | |
| I2C SDA | 4 | Reverted to as-built (OLED removed from bring-up) |
| I2C SCL | 13 | Reverted to as-built |

### Pin changes made and then reverted

An experiment moved `LORA_NSS` 21→13 and I2C 4/13→21/22, on the theory that a bad chip-select was causing the `-2`. The diagnostic disproved this (the chip was alive and NSS was working). **All changes were reverted** in all five `config.h` files rather than stacking further changes on a failed hypothesis.

> ⚠️ **Unresolved documentation conflict:** `SWAP_Hardware_Reference.md` lists **SDA=13, SCL=4**, while all `config.h` files use **SDA=4, SCL=13**. These contradict each other; one has been wrong throughout. Verify against the physical board and correct whichever is wrong.

---

## 9. Corrections Made During This Session

Recorded honestly, since two of them cost real bench time:

1. **"All-identical bytes = floating MISO"** — wrong. The SX126x shifts its status byte out on MISO during nearly any transaction, so a chip rejecting every command returns the same byte repeatedly. This looks identical to a dead line but means the opposite. `0x2A` was diagnostic from the very first run; decoding it earlier would have skipped several rounds of wire-chasing.
2. **MISO pull-up/pull-down test** — invalid. Calling `pinMode()` on a pin already bound to the SPI peripheral detaches it from the ESP32 GPIO matrix, so both reads returned `0x00` regardless of the module. The test was removed.
3. **NSS→GPIO13 pin move** — a hypothesis the diagnostic later disproved. It may itself have introduced a second fault by disturbing working wiring. Reverted.
4. **"MOSI is unlikely"** — stated early on the reasoning that a broken MOSI would still leave the chip driving *varying* data. Exactly backwards: it drives constant status.

**Process lesson:** prove with the diagnostic before changing pin assignments. Reasoning from symptoms alone produced three wrong hypotheses; decoding one status byte produced the right answer.

---

## 10. Roadmap Status

| Item | State |
|---|---|
| 1. Two-way LoRa ack | ✅ Code complete — blocked on hardware for verification |
| 2. Fix UART baud mismatch (19200 → 115200) | ⬜ Not started |
| 3. Merge into full adaptive switching | ⬜ Not started |
| 4. Wire Node A into UNO Q pipeline | ⬜ Not started |
| 5. Validate/retrain on real data | ⬜ Not started |

> **Carry-forward for Item 3:** `swap_node`, `swap_node_a` and `swap_node_b` all contain a byte-for-byte copy of the *original* `lora_link.cpp`, still carrying the missing-`standby()` bug from §4.1. They haven't been bitten yet only because they've never been run continuously on real hardware. Apply the same fixes before Item 3.

---

## 11. Files Changed

**Modified**
- `lora_bringup_a/{lora_bringup_a.ino, config.h, lora_link.cpp}`
- `lora_bringup_b/{lora_bringup_b.ino, config.h, lora_link.cpp}`
- `swap_node/config.h`, `swap_node_a/config.h`, `swap_node_b/config.h` *(changed, then reverted)*

**Created**
- `lora_spi_diag/lora_spi_diag.ino` — raw SPI/BUSY diagnostic (v5)
- `pin_integrity/pin_integrity.ino` — GPIO integrity test
- `hello_serial/hello_serial.ino` — serial sanity check

**Removed**
- `lora_bringup_a/_to_delete/oled_display.{cpp,h}`
- `lora_bringup_b/_to_delete/oled_display.{cpp,h}`

---

## 12. References

- [RadioLib #575 — SX1262 receive error, `standby()` recovery](https://github.com/jgromes/RadioLib/issues/575)
- [RadioLib #1165 — `startReceive` fails with code -2](https://github.com/jgromes/RadioLib/discussions/1165)
- [RadioLib SX1262 class reference — `begin()` signature](https://jgromes.github.io/RadioLib/class_s_x1262.html)
- [SX126x status byte encoding](https://pkg.go.dev/tinygo.org/x/drivers/sx126x)
- [ESP32 GPIO20 not bonded out on WROOM-32](https://esp32.com/viewtopic.php?t=426)
