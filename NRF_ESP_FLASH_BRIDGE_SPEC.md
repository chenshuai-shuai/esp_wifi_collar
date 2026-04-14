# NRF <-> ESP32-C3 Flash Bridge Spec

## Goal

Enable a PC host to flash the ESP32-C3 firmware through the nRF MCU over a UART bridge, while the nRF also controls ESP32-C3 boot mode and reset pins.

This document is the interface contract. The ESP workspace may change here, but the nRF repository should be updated manually on the Windows side to match this spec.

## Hardware Assumptions

ESP target:
- Module: `ESP32-C3-WROOM-02U-N4`
- ROM download interface: `UART0`

nRF to ESP wiring:
- `nRF P0.28 -> ESP WIFI_RXD`
- `nRF P0.29 <- ESP WIFI_TXD`
- `nRF P1.02 -> NRF_BOOT_CTRL -> ESP GPIO9`
- `nRF P1.04 -> NRF_WIFIEN_CTRL -> ESP EN`

Meaning:
- `GPIO9=0` during reset enters ESP32-C3 UART download mode
- `GPIO9=1` or released during reset boots normally from flash
- `EN` low then high resets the ESP

## Required nRF UART Topology

The nRF firmware needs two UART roles:

1. Host UART
- Connected to the PC
- Used for command input and raw flashing data

2. ESP UART
- Connected to ESP32-C3 `UART0`
- Used to forward flashing traffic to the ESP ROM bootloader

Recommended mapping for the current design:
- `uart0`: PC host side
- `uart1`: ESP side

ESP-side pin direction must be:
- `uart1 TX -> P0.28`
- `uart1 RX -> P0.29`

This is important because the existing design intent is:
- PC -> nRF host UART -> nRF ESP UART -> ESP ROM bootloader

## Required nRF Control Commands

The host must be able to send ASCII line commands to the nRF firmware:

- `NRF:ESP_DL`
  - Enter ESP download mode
  - Sequence:
    1. Drive ESP `GPIO9` low
    2. Drive ESP `EN` low
    3. Delay 20-50 ms
    4. Release ESP `EN` high
    5. Delay 100-300 ms
    6. Enable host UART <-> ESP UART transparent forwarding

- `NRF:ESP_BOOT`
  - Enter ESP normal boot mode
  - Sequence:
    1. Release ESP `GPIO9` high or high-Z
    2. Drive ESP `EN` low
    3. Delay 20-50 ms
    4. Release ESP `EN` high
    5. Disable flashing bridge mode if active

- `NRF:ESP_RST`
  - Reset ESP without changing the latched boot mode intent
  - Sequence:
    1. Drive ESP `EN` low
    2. Delay 20-50 ms
    3. Release ESP `EN` high

Optional debug response strings are helpful, for example:
- `OK:ESP_DL`
- `OK:ESP_BOOT`
- `OK:ESP_RST`
- `ERR:<reason>`

## Required nRF Bridge Behavior

After receiving `NRF:ESP_DL`, the nRF must switch into a transparent bridge mode:

- Every byte received from the host UART is forwarded to the ESP UART
- Every byte received from the ESP UART is forwarded to the host UART
- No command parsing should interfere with raw esptool traffic once bridge mode is active

Recommended implementation detail:
- Only parse command lines when bridge mode is inactive
- When bridge mode is active, treat both UARTs as raw byte streams

Recommended session exit behavior:
- If host traffic is idle for about `1500-3000 ms`, automatically:
  1. Disable bridge mode
  2. Release `GPIO9`
  3. Reset ESP back into normal boot

This lets the same host port be reused for:
- `esptool` / `idf.py flash`
- optional post-flash application monitoring

## Host Flash Flow

Expected PC-side flow:

1. Open the nRF host serial port
2. Send `NRF:ESP_DL\n`
3. Wait about `0.3-1.0 s`
4. Start ESP flashing over the same serial port
5. After flashing, either:
   - let nRF auto-return ESP to normal boot after idle, or
   - explicitly send `NRF:ESP_BOOT\n`

## ESP Flash Arguments

Current ESP project output should be flashed as:

```text
0x0      build/bootloader/bootloader.bin
0x8000   build/partition_table/partition-table.bin
0x10000  build/esp_wifi_collar.bin
```

Example host command:

```bash
esptool.py --chip esp32c3 --port <NRF_HOST_PORT> --baud 115200 write_flash \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/esp_wifi_collar.bin
```

Or:

```bash
idf.py -p <NRF_HOST_PORT> -b 115200 flash
```

## nRF Repository Changes Needed On Windows

These are the changes that should be made manually in the nRF workspace on Windows.

### 1. Fix ESP UART pin mapping

In the nRF devicetree overlay:
- Ensure the ESP-facing UART TX pin is `P0.28`
- Ensure the ESP-facing UART RX pin is `P0.29`

If using `uart1` for ESP:
- `uart1 TX = P0.28`
- `uart1 RX = P0.29`

Do not leave ESP-side TX disconnected.

### 2. Separate host UART and ESP UART

In the nRF firmware:
- One UART must talk to the PC host
- One UART must talk to the ESP

If current code uses the same UART for command handling and ESP wiring, split it.

Recommended:
- `uart0` for host
- `uart1` for ESP

### 3. Add transparent bridge mode

In the nRF app logic:
- Keep line-command parsing before bridge mode starts
- After `NRF:ESP_DL`, stop interpreting incoming bytes as commands
- Forward raw bytes both directions until the flash session ends

### 4. Keep the GPIO mode control API

The existing nRF GPIO control concept is correct and should remain:
- download mode control through `NRF_BOOT_CTRL`
- reset/enable control through `NRF_WIFIEN_CTRL`

Only the timing and bridge integration need to match this spec.

### 5. Update the Windows-side helper script

The helper script should:
- send `NRF:ESP_DL`
- wait briefly
- invoke `idf.py flash` or `esptool.py write_flash` on the same host port
- optionally wait for auto-return to normal boot
- optionally open monitor

## Verification Checklist

The interface is considered aligned when all of the following are true:

1. nRF receives `NRF:ESP_DL` and ESP enters ROM download mode
2. `esptool.py chip_id` works through the nRF host port
3. `idf.py flash` completes through the nRF host port
4. After flashing, ESP reboots normally from flash
5. Optional: `idf.py monitor` over the same port can observe ESP logs

## First Bring-Up Recommendation

Before full flashing, verify in this order:

1. `NRF:ESP_BOOT` can make ESP start normally
2. `NRF:ESP_DL` can make `esptool.py chip_id` succeed
3. small `idf.py flash` test succeeds
4. full application flash succeeds

This reduces variables and makes wiring or timing issues much easier to debug.
