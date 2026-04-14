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

## Notes

- The runtime scaffold uses static task and queue allocation where practical.
- Service and app tasks now use `tskNO_AFFINITY`, which keeps the code valid on the
  single-core ESP32-C3 while remaining portable across other ESP-IDF targets.
- The original Wi-Fi scan example code has been removed; README and menuconfig entries
  now reflect the collar controller scaffold instead of the upstream example.
