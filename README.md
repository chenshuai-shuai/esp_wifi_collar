# ESP32-C3 Wi-Fi Collar Controller

This project started from the ESP-IDF Wi-Fi scan example and has been refactored into
an ESP32-C3 application scaffold for the `ESP32-C3-WROOM-02U-N4` module.

The current startup flow is:

`NVS -> BSP -> kernel -> services -> app`

## Project Layout

- `main/`: unified application entry point
- `components/platform_hal/`: low-level platform bring-up
- `components/bsp/`: board abstraction
- `components/kernel/`: message bus, workqueue, supervisor, RT task scaffold
- `components/services/`: service dispatch and deferred work
- `components/app/`: application task manager

## Target

This repository is now configured for `esp32c3`.

Set up the toolchain and build with:

```bash
idf.py set-target esp32c3
idf.py build
```

Flash with:

```bash
idf.py -p PORT flash monitor
```

For the shared `nRF + ESP UART0` flashing flow, use:

```bash
python3 tools/nrf_shared_uart_flash.py --port /dev/ttyUSB0 probe
python3 tools/nrf_shared_uart_flash.py --port /dev/ttyUSB0 flash
python3 tools/nrf_shared_uart_flash.py --port /dev/ttyUSB0 flash --monitor
python3 tools/nrf_shared_uart_flash.py --port /dev/ttyUSB0 send NRF:ESP_DL
```

This helper sends `NRF:ESP_DL` and `NRF:ESP_BOOT` around the ESP32-C3 flashing step, so the
PC still flashes the ESP directly over the shared UART while the nRF only controls boot mode.

## Notes

- The runtime scaffold uses static task and queue allocation where practical.
- Service and app tasks now use `tskNO_AFFINITY`, which keeps the code valid on the
  single-core ESP32-C3 while remaining portable across other ESP-IDF targets.
- The original Wi-Fi scan example code has been removed; README and menuconfig entries
  now reflect the collar controller scaffold instead of the upstream example.
- `ESP32-C3-WROOM-02U-N4` uses 4 MB flash, and this project now emits flash headers for 4 MB.
