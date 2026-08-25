#ifndef SWAP_CONFIG_H
#define SWAP_CONFIG_H

#include <Arduino.h>

// --- LoRa (SX1262) pin map ---
// Same physical pin map as swap_node_a/ and swap_node_b/'s config.h (verified
// against the real Node A/B ESP32 DevKit V1 pin diagram). This is a
// standalone copy for this minimal bring-up test -- if the wiring ever
// changes, update both.
//
// LORA_NSS: REVERTED to GPIO21 (2026-08-23).
// It was briefly moved to GPIO13 on the theory that a bad chip-select was
// causing begin() to fail findChip() with -2. lora_spi_diag then proved
// that theory wrong: the module powers up, answers the RST pulse, and
// drops BUSY in 2ms -- it is alive. GPIO21 is also the pin that carried
// 25 successful exchanges on the very first hardware run, so it is the
// known-good configuration. Reverting removes a variable rather than
// stacking another change on a failed hypothesis.
// NOTE: with NSS back on 21, the I2C bus returns to its as-built GPIO4/13.
#define LORA_NSS 21    // SPI_NSS / LoRa CS
#define LORA_RST 33    // LORA_RESET
#define LORA_BUSY 27   // LORA_BUSY
#define LORA_DIO1 26   // LORA_DIO1 (IRQ)
#define LORA_RXEN 25   // LORA_RXEN (must be driven in firmware)
#define LORA_TXEN 32   // LORA_TXEN (must be driven in firmware)

// --- LoRa radio parameters (IN865 region defaults) ---
// PWR is a conservative default, not a regulatory certification — check
// your local IN865 EIRP limit before increasing it.
#define LORA_FREQ 865.0     // MHz
#define LORA_BW 125.0       // kHz
#define LORA_SF 9
#define LORA_CR 7            // 4/7
#define LORA_SYNC 0x12       // RadioLib private-network sync word
#define LORA_PWR 14          // dBm
#define LORA_PREAMBLE_LEN 8  // matches RadioLib's own default, made explicit
// Core1262-HF's TCXO is always 1.6V (hardware fact, not tunable) -- passed
// explicitly to lora.begin() rather than relying on RadioLib's own default,
// per the "never assume defaults" hardware note. RadioLib's current default
// also happens to be 1.6V, but a future library version changing that
// default would silently break TCXO startup on this exact module if we
// were relying on it implicitly.
#define LORA_TCXO_VOLTAGE 1.6f
// IN865 primary sub-band duty cycle allowance.
#define MAX_LORA_DUTY_CYCLE 0.01f

#endif
