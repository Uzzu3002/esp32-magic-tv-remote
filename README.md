# ESP32 Magic TV Remote

Turn an ESP32 into a "magic remote" for your TV: your phone becomes a touchpad that moves a mouse cursor on the TV, plus arrow keys, volume, media keys and text typing for search boxes.

The ESP32 pairs with the TV as a **Bluetooth mouse + keyboard**, and serves a remote-control web page on your home Wi-Fi. No app to install. Tested on a Samsung UA43N5470 (2018 Tizen smart TV).

## Sketches

| Folder | What it does | Needs |
|---|---|---|
| [`BleTvMouse`](BleTvMouse/BleTvMouse.ino) | **Main project.** Bluetooth mouse/keyboard for the TV + phone touchpad page | TV with Bluetooth |
| [`SamsungIrRemote`](SamsungIrRemote/SamsungIrRemote.ino) | Samsung IR remote controlled from a phone page on home Wi-Fi (power, volume, numbers...) | IR LED |
| [`MagicRemote`](MagicRemote/MagicRemote.ino) | Standalone multi-brand IR remote (Samsung, LG, Sony, Panasonic, Philips) on its own Wi-Fi hotspot | IR LED |

## Features (BleTvMouse)

- Drag on the phone touchpad to move the TV cursor, tap to click, scroll buttons
- Arrows, OK, Back, Home, Play/Pause, Volume, Mute (Bluetooth HID + consumer keys)
- Type text on the phone and send it to the TV's search box
- Power button over IR (Bluetooth can't wake a sleeping TV)
- First-boot Wi-Fi setup page; reachable at `http://magicremote.local`

## Hardware

- ESP32 dev board (classic ESP32, e.g. 38-pin DevKit)
- Optional, for IR power: 940 nm IR LED + 100 Ω resistor

```
G4  ──[100 Ω]──►|── GND
              IR LED (long leg towards the resistor)
```

## Setup

1. Flash `BleTvMouse` (see Build below).
2. On your phone, join the Wi-Fi **MagicRemote-Setup** → the setup page opens (or go to `192.168.4.1`) → choose your home Wi-Fi and enter its password.
3. Pair on the TV: **Settings → General → External Device Manager → Input Device Manager → Bluetooth Device List → MagicRemote**.
4. With your phone on the home Wi-Fi, open `http://magicremote.local` (or the IP printed on the serial monitor).

Hold the **BOOT** button for 5 s to forget the Wi-Fi and run setup again.

## Build

Uses [arduino-cli](https://arduino.github.io/arduino-cli/) with the Espressif ESP32 core (3.x).

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install NimBLE-Arduino WebSockets IRremoteESP8266
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,FlashFreq=40,FlashMode=dio BleTvMouse
arduino-cli upload -p COM4 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,FlashFreq=40,FlashMode=dio BleTvMouse
```

`huge_app` is needed because Wi-Fi + Bluetooth together don't fit the default partition.
`FlashFreq=40` fixed crashes and an "image is corrupt" boot loop on a board whose flash misread at 80 MHz once Wi-Fi and Bluetooth were both running; you may not need it.

## Notes

- On Samsung TVs the mouse cursor works in the browser and most menus; some apps (YouTube, Netflix) ignore the mouse, so use the arrow keys there.
- The IR sketches use standard Samsung NEC-style codes (`0xE0E0....`), which work on most Samsung TVs.
