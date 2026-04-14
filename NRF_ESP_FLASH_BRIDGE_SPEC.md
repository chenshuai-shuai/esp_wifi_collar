# NRF <-> ESP32-C3 Flash Control Spec

## Goal

Enable a PC host to flash the ESP32-C3 firmware while the nRF MCU controls ESP32-C3 boot mode and reset pins.

This design uses a shared ESP UART0 line.
The nRF is a boot/download mode controller, not a full transparent UART bridge.

## Hardware Assumptions

ESP target:
- Module: `ESP32-C3-WROOM-02U-N4`
- ROM download interface: `UART0`

nRF to ESP wiring:
- `nRF P0.28 -> ESP WIFI_RXD`
- `nRF P0.29 <- ESP WIFI_TXD`
- `nRF P1.02 -> NRF_BOOT_CTRL -> ESP GPIO9`
- `nRF P1.04 -> NRF_WIFIEN_CTRL -> ESP EN`

PC host wiring through USB-TTL:
- `TTL TX -> P0.28`
- `TTL RX -> P0.29`
- `TTL GND -> board GND`

Meaning:
- `GPIO9=0` during reset enters ESP32-C3 UART download mode
- `GPIO9=1` or released during reset boots normally from flash
- `EN` low then high resets the ESP

## UART Topology

This design does not use two independent UART channels inside nRF for host-side bridge forwarding.

Actual topology is:

- PC host USB-TTL is directly connected to the shared ESP UART0 lines
- ESP32-C3 uses these same lines for ROM download and runtime logs
- nRF is attached to the same lines only for command detection and boot mode control

The nRF does not act as the primary flashing transport.
The PC still talks directly to the ESP ROM bootloader over the shared UART0 line.

## Required nRF Control Commands

The host must be able to send ASCII line commands that the nRF can detect on the shared UART line:

- `NRF:ESP_DL`
  - Enter ESP download mode
  - Sequence:
    1. Drive ESP `GPIO9` low
    2. Drive ESP `EN` low
    3. Delay about `20-80 ms`
    4. Release ESP `EN` high
    5. Wait about `100-300 ms`

- `NRF:ESP_BOOT`
  - Enter ESP normal boot mode
  - Sequence:
    1. Release ESP `GPIO9` high or high-Z
    2. Drive ESP `EN` low
    3. Delay about `20-80 ms`
    4. Release ESP `EN` high
    5. Wait about `100-300 ms`

- `NRF:ESP_RST`
  - Reset ESP without changing the current boot mode intent
  - Sequence:
    1. Drive ESP `EN` low
    2. Delay about `20-80 ms`
    3. Release ESP `EN` high

Optional debug response strings are helpful, for example:
- `OK:ESP_DL`
- `OK:ESP_BOOT`
- `OK:ESP_RST`
- `ERR:<reason>`

## nRF Behavior

The nRF firmware only needs to do these jobs:

1. Monitor the shared UART RX path for control commands
2. Toggle `NRF_BOOT_CTRL` and `NRF_WIFIEN_CTRL`
3. Leave normal ESP flashing traffic untouched after mode switching

Important behavior:
- Only the `NRF:ESP_*` commands are interpreted by nRF
- Raw `esptool.py` / `idf.py flash` traffic is not bridged or modified by nRF
- After mode switch is complete, the PC continues to talk directly to ESP UART0

## Host Flash Flow

Expected PC-side flow:

1. Open the shared serial port
2. Send `NRF:ESP_DL\n`
3. Wait about `0.3-1.0 s`
4. Start ESP flashing over the same serial port
5. After flashing, send `NRF:ESP_BOOT\n`
6. Wait briefly for reboot
7. Open monitor if needed

## ESP Flash Arguments

Current ESP project output should be flashed as:

```text
0x0      build/bootloader/bootloader.bin
0x8000   build/partition_table/partition-table.bin
0x10000  build/esp_wifi_collar.bin
```

## Verification Checklist

The interface is considered aligned when all of the following are true:

1. `NRF:ESP_BOOT` can make ESP start normally
2. `NRF:ESP_DL` can make `esptool.py --chip esp32c3 chip_id` succeed
3. ESP flashing completes through the shared UART
4. After flashing, `NRF:ESP_BOOT` makes ESP reboot normally from flash
5. Optional: serial monitor can observe ESP logs on the same shared UART
