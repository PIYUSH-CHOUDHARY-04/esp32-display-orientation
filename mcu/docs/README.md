# Reference material

**Not committed.** These are the vendors' documents, not this project's, and they get revised —
a copy checked in here would be stale within a year and wrong without saying so.

Download whichever you need into this directory. It's gitignored, so they'll stay out of your way.

| Part               | Document                                                                                         |
| ------------------ | ------------------------------------------------------------------------------------------------ |
| **ESP32**          | [Datasheet](https://www.espressif.com/en/support/documents/technical-documents)                  |
| **ESP32**          | [Technical Reference Manual](https://www.espressif.com/en/support/documents/technical-documents) |
| **ESP32-WROOM-32** | [Module datasheet](https://www.espressif.com/en/support/documents/technical-documents)           |
| **BMA400**         | [Datasheet](https://www.bosch-sensortec.com/products/motion-sensors/accelerometers/bma400/)      |
| **PCF8574**        | [Datasheet](https://www.ti.com/product/PCF8574)                                                  |

## What you actually need them for

- **BMA400** — the register map, and the activity/inactivity interrupt configuration. `sensor.c`
  drives the interrupt path rather than polling, and the thresholds and durations in there come
  straight out of this document.
- **PCF8574** — the I²C backpack behind the LCD. One byte in, eight pins out; the bit order is the
  only thing you need from it, and `lcd.c` documents the mapping it uses.
- **ESP32 TRM** — GPIO, SPI, and the interrupt allocator. Rarely needed; ESP-IDF's own docs cover
  almost everything.

The pinout reference most people actually want is
[lastminuteengineers.com/esp32-pinout-reference](https://lastminuteengineers.com/esp32-pinout-reference/),
which is clearer than any of the above for wiring things up.
