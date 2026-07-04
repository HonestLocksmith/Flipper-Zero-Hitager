# Hitager FAP MVP

Minimal Flipper Zero external app that talks ASCII UART to an Arduino/Nano Hitager.

## Wiring

Flipper GPIO UART is 3.3 V logic. Use a level shifter if your Arduino TX is 5 V.

- Flipper TX -> Arduino RX
- Flipper RX -> Arduino TX
- Flipper GND -> Arduino GND

Default serial: 115200 baud, 8N1.

## Controls

- OK: send `i05C0` read ID
- Up: send `o` RF ON
- Down: send `f` RF OFF
- Right: send `v` version
- Left: send raw preset string stored in `HITAGER_RAW_PRESET`
- Back: exit

The app reads lines until `EOF`, and extracts `RESP:` data like the Windows Hitager PortHandler.

## Build

Copy this folder to your firmware tree:

```bash
cp -r hitager_fap ~/flipperzero-firmware/applications_user/hitager
cd ~/flipperzero-firmware
./fbt fap_hitager
```

Or build by source path:

```bash
./fbt build APPSRC=applications_user/hitager
```

The output FAP will be in the firmware build output tree, usually under `dist/f7-D/.extapps/GPIO/Hitager.fap` or similar depending on firmware branch.

## Notes

This is intentionally transport-only. The Arduino owns PCF7991 timing and decoding. Flipper only sends commands and displays replies.
