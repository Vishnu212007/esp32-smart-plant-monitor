# ESP32 Smart Plant Monitor

A standalone plant monitoring system built on the ESP32. It reads soil moisture and ambient light, shows them on an OLED display, and serves a live dashboard over Wi-Fi. The dashboard works fully offline, with no cloud and no CDN.

## Features
- Soil moisture (analog) and light level (BH1750, in lux)
- 128x64 SSD1306 OLED with two auto-rotating pages
- Offline web dashboard with live charts and 30 minutes of history
- Dry / Good / Wet status and a light-level label
- Calibrate dry and wet values from the browser (saved in flash)
- Adjustable dry threshold
- Wi-Fi fallback hotspot if the router is not found
- mDNS address: `http://plantmonitor.local`
- Optional relay-driven auto-watering with cooldown protection

## Hardware
| Part | Notes |
|---|---|
| ESP32 dev board | Any common ESP32 DevKit |
| Capacitive soil moisture sensor | Analog output |
| BH1750 light sensor | I2C |
| SSD1306 OLED 128x64 | I2C, address 0x3C |
| Relay + pump (optional) | Use a separate power supply for the pump |

## Wiring
| Component | ESP32 pin |
|---|---|
| OLED / BH1750 SDA | GPIO 21 |
| OLED / BH1750 SCL | GPIO 22 |
| Soil sensor AO | GPIO 34 |
| Relay IN (optional) | GPIO 26 |
| VCC / GND | 3.3V / GND |

## Libraries
- Adafruit GFX
- Adafruit SSD1306
- BH1750 (Christopher Laws)

## Setup
1. Open `esp32_plant_monitor.ino` in the Arduino IDE.
2. Set `WIFI_SSID` and `WIFI_PASSWORD` at the top of the file.
3. Select your ESP32 board and upload.
4. Open the IP shown on the OLED, or `http://plantmonitor.local`.
5. Calibrate: hold the sensor in air and tap "Calibrate DRY", then put it in water and tap "Calibrate WET".

If the router is not found, connect to the `PlantMonitor` hotspot (password `12345678`) and open `192.168.4.1`.

## Optional: auto-watering
Set `#define ENABLE_PUMP 1` in the sketch. The pump runs for 5 seconds when the soil is below the dry threshold, with a 10-minute cooldown between runs. Power the pump from a separate supply, not the ESP32.

## Roadmap
- [ ] Telegram / push alerts
- [ ] SD-card or cloud data logging
- [ ] Temperature and humidity sensor
- [ ] Deep-sleep battery mode

## License
MIT
