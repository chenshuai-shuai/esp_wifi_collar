# ESP32-C3 <-> nRF52840 System Contract

## Current Integration Status

The nRF side already owns the ESP boot/download control flow and can trigger ESP flashing from the PC side.

The currently observed nRF implementation uses:
- nRF `uart0` as the host-facing command/flash port
- nRF `uart1` as the ESP-facing UART bridge
- GPIO control for ESP `EN` and `GPIO9`

For ESP application development, this means the ESP project should treat the following resources as reserved.

## Reserved ESP Resources

### UART0

ESP `UART0` is reserved for:
- ROM download
- firmware flashing
- early boot logs
- runtime logs routed through the nRF flashing path

Implication:
- do not repurpose ESP `UART0` pins for application payload traffic
- keep console on `UART0`
- keep the default 115200 path available during bring-up

### GPIO9

ESP `GPIO9` is used by the nRF side to select download mode versus normal boot.

Implication:
- do not use `GPIO9` for application peripherals
- do not add runtime logic that drives or depends on `GPIO9`

### EN / CHIP ENABLE

ESP `EN` is controlled externally by the nRF side.

Implication:
- application code must tolerate external resets
- boot path should be deterministic and idempotent

## Current UART Application Control Contract

The ESP project now contains an optional ASCII line-command control module for the same `UART0`
that is already used for boot logs and flashing.

This gives the nRF side a software-only way to ask the ESP to re-enter Wi-Fi provisioning mode
without adding a physical button.

Current status:
- the ESP-side control module exists
- it is intentionally not started by default in the current application startup path
- enabling it later should be done explicitly by calling `wifi_control_service_start()`

### Transport

- UART: `ESP UART0`
- Encoding: ASCII
- Baud rate: keep aligned with the existing bring-up path, currently `115200`
- Line ending: `\n` or `\r\n`

### Supported ESP Commands

- `ESP:WIFI_REPROVISION`
  - Purpose:
    Clear stored Wi-Fi credentials and force the ESP back into SoftAP provisioning mode
  - ESP behavior:
    1. Erase saved `ssid` and `password` from NVS namespace `wifi_portal`
    2. Disconnect current STA link if connected
    3. Start the SoftAP provisioning portal again
    4. Publish Wi-Fi state `provisioning`

### ESP Responses

- Success:
  - `OK:WIFI_REPROVISION`
- Failure:
  - `ERR:WIFI_REPROVISION:<esp_err_name>`

Examples:
- `ERR:WIFI_REPROVISION:ESP_ERR_NVS_NOT_FOUND`
- `ERR:WIFI_REPROVISION:ESP_FAIL`

### Important Parsing Note

`UART0` still carries regular ESP runtime logs.
That means the nRF side must not assume the UART stream contains only command responses.

Recommended parser behavior:
- treat UART as a line stream
- search for exact line prefixes `OK:` and `ERR:`
- ignore unrelated log lines such as `I (...) wifi_svc: ...`

### Recommended nRF Usage Flow

When the product needs to force the user back into Wi-Fi setup:

1. Send `ESP:WIFI_REPROVISION\n`
2. Wait for either `OK:WIFI_REPROVISION` or `ERR:WIFI_REPROVISION:<...>`
3. If success is returned, tell the upper layer that the ESP should now expose the provisioning hotspot
4. Optionally watch ESP logs for:
   - `Entered provisioning mode via UART command`
   - `SoftAP portal active: ssid=... ip=192.168.4.1`

## ESP Application Design Rules

1. Boot fast and log clearly
- emit a short boot banner
- print build/version info
- print major subsystem init success/failure

2. Avoid claiming pins that are part of the update path
- especially boot strap and UART0-related pins

3. Make service startup modular
- microphone/audio capture
- network bring-up
- realtime dialog session
- command/control link with nRF

4. Prefer explicit health reporting
- startup state
- Wi-Fi state
- audio pipeline state
- cloud/session state

## Recommended ESP Development Order

1. Solidify board-level pin plan
- reserve flash/update pins
- map audio-related pins that really belong to ESP

2. Add a minimal runtime identity layer
- app name
- firmware version
- boot reason
- reset counter

3. Add Wi-Fi bring-up service
- credentials source
- connect / reconnect
- state logs

4. Add audio hardware abstraction
- speaker amp enable
- I2S output
- future microphone path if moved to ESP

5. Add realtime dialog service skeleton
- transport abstraction
- session lifecycle
- streaming state machine

## Immediate Next Step

Before adding the dialog stack itself, the ESP project should first gain:
- a device identity/version module
- a structured boot log
- a Wi-Fi service skeleton

That gives us a stable base to debug networking and remote session startup before audio complexity is added.
