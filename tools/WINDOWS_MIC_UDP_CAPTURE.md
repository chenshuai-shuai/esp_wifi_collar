# Windows Host Microphone UDP Capture

This document is for the Windows host teammate. The ESP32 firmware is already configured to send live microphone PCM to:

- Host: `192.168.1.2`
- Port: `3333`
- Format: `PCM s16le`
- Sample rate: `24000 Hz`
- Channels: `1`

The ESP32 starts sending once microphone capture is running and Wi-Fi routing is available.

## What You Need

- Windows host IP is `192.168.1.2`
- Python 3 installed on Windows
- Optional: `ffplay` if you want live playback while recording

## Verify Python

Open PowerShell and run:

```powershell
py --version
```

If that fails, try:

```powershell
python --version
```

## Files Needed

Copy this script from the repo or shared workspace onto the Windows host:

- `tools/mic_udp_capture.py`

No third-party Python packages are required.

## Start Receiving Audio

Open PowerShell in the folder containing `mic_udp_capture.py` and run:

```powershell
py .\mic_udp_capture.py --port 3333 --rate 24000 --channels 1 --wav .\mic_capture.wav
```

If `ffplay` is installed and on `PATH`, you can also play audio live:

```powershell
py .\mic_udp_capture.py --port 3333 --rate 24000 --channels 1 --wav .\mic_capture.wav --play
```

Expected startup output:

```text
Listening on udp://0.0.0.0:3333 -> mic_capture.wav (24000 Hz, 1 ch, 16-bit)
```

Expected receive progress:

```text
packets=50 bytes=48000 seconds=1.00 last_from=192.168.1.xxx:yyyyy
```

## Stop and Save WAV

Press `Ctrl+C` in PowerShell.

The script finalizes the WAV file on exit. Then play it with:

```powershell
start .\mic_capture.wav
```

## Windows Firewall

If Windows prompts whether to allow Python on the network, click Allow.

If no packets arrive:

- Confirm the host IP is still `192.168.1.2`
- Confirm UDP port `3333` is not blocked
- Confirm the ESP32 log shows:

```text
Mic UDP stream ready: dst=192.168.1.2:3333 format=pcm_s16le rate=24000 channels=1
```

- Confirm the ESP32 `Mic test:` log shows UDP counters increasing, for example:

```text
udp=50/48000 fail=0
```

## Troubleshooting

- If packets do not appear, temporarily disable Windows firewall for a quick test or add an inbound UDP allow rule for port `3333`.
- If `--play` does not work, remove it and just record the WAV first.
- If the PC IP changes, the ESP32 firmware must be rebuilt or `sdkconfig` updated to the new host IP.
