# ⚙️ DIY Flipper Zero

> WARNING: I do not take responsibility if you damage your board or property. This guide is for educational purposes only — proceed at your own risk.

---

## 📚 Table of contents
- [⚙️ DIY Flipper Zero](#️-diy-flipper-zero)
  - [📚 Table of contents](#-table-of-contents)
  - [Summary](#summary)
  - [What works / Limitations](#what-works--limitations)
  - [Key pins \& wiring (quick reference)](#key-pins--wiring-quick-reference)
  - [MCP23017 — Buttons \& RGB wiring guide](#mcp23017--buttons--rgb-wiring-guide)
    - [Button-to-MCP mapping (from firmware)](#button-to-mcp-mapping-from-firmware)
  - [How to flash (OTP + firmware)](#how-to-flash-otp--firmware)
  - [Notes \& tips](#notes--tips)
  - [📖 Additional documentation](#-additional-documentation)
  - [Credits](#credits)
  - [Currently busy with a high-priority production release: baby\_v1.0 🧑‍🍼](#currently-busy-with-a-high-priority-production-release-baby_v10-)
  - [☕ Support this project](#-support-this-project)

## Summary
This target implements a Flipper-style board based on the `STM32WB55CGU6` and integrates the following external hardware:

- <a href="mics/IMG_20260201_161815.JPG"><img src="mics/IMG_20260201_161815.JPG" alt="Prototype board photo" width="480" style="max-width:100%; height:auto;"></a>
- <p><em>Figure 1 — Prototyp</em></p>

- ✅ I2C OLED display (SH1106 / SSD1306)
- ✅ INA219 (battery/current monitor)
- ✅ MCP23017 (I/O expander for buttons, RGB, vibro)
- ✅ microSD (SPI)
- ✅ CC1101 sub-GHz module
- ✅ Buttons (handled via MCP23017)
- ✅ RGB status LED (via MCP23017)
- ✅ Speaker / buzzer (TIM16)
- ✅ IR RX/TX
- ✅ Vibration motor (via MCP23017)
- ✅ Li-ion battery + optional 3.7→5V boost

## What works / Limitations
- ✅ Most official Flipper features are implemented.
- ✅ I2C: INA219, MCP23017, OLED (I2C preferred to free SPI).
- ✅ IR (RX/TX) and speaker/vibro outputs work.
- ✅ MCP23017 handles buttons, RGB LED and vibro (pins B1/B2/B3, B0 respectively).
- ✅ SD card over SPI is supported (CS on PA10).

- ⚠️ NFC won't work.

## Key pins & wiring (quick reference)
Important: these macros are defined in `furi_hal_resources.*` and are used across the HAL code.

| Component | Bus / Interface | MCU pin (macro) | Notes |
|---|---:|---|---|
| I2C (power/default, I2C1) | I2C1 | SCL: PA9 (`I2C_1_SCL_GPIO_Port`/`I2C_1_SCL_Pin`)<br/>SDA: PB9 (`I2C_1_SDA_GPIO_Port`/`I2C_1_SDA_Pin`) | Used by INA219, default MCP23017 and oled screen |
| I2C (external, I2C3) | I2C3 | SCL: PA7 (`I2C_3_SCL_GPIO_Port`/`I2C_3_SCL_Pin`)<br/>SDA: PB4 (`I2C_3_SDA_GPIO_Port`/`I2C_3_SDA_Pin`) | Useful for external I2C |
| SPI1 (shared) | SPI1 | MISO: PA6 (`SPI_MISO_Pin`), MOSI: PB5 (`SPI_MOSI_Pin`), SCK: PB3 (`SPI_SCK_Pin`) | CC1101 and SD share this bus |
| CC1101 | SPI + IRQ | CS: PA15 (`CC1101_CS_Pin`), G0: PA1 (`CC1101_G0_Pin`) | Module IRQ on G0 |
| SD card | SPI | CS: PA10 (`SD_CS_Pin`) | SD on SPI; slow/fast presets available |
| MCP23017 | I2C | INT: PB0 (`MCP_INT_Pin`) | Default I2C address 0x20; RGB pins B1/B2/B3 (MCP pins 9/10/11); vibro on B0 (pin 8) |
| IR | GPIO / ALT | RX: PA0 (`IR_RX_Pin`), TX: PA8 (`IR_TX_Pin`) | TX is IR LED drive — use proper resistor/transistor |
| Speaker | PWM | PB8 (`SPEAKER_Pin`) — TIM16 | Use transistor/amplifier if needed |
| iButton | 1-Wire | PA3 (`iBTN_Pin`) | |
| NFC CS | SPI | PE4 (`NFC_CS_Pin`) | Verify wiring & driver integration |
| UART | USART1 | TX: PB6, RX: PB7 | Debug / serial |
| USB | USB | DM/DP: PA11 / PA12 | USB lines handled by HAL init code |

See the canonical pin macros in [targets/f7/furi_hal/furi_hal_resources.h](targets/f7/furi_hal/furi_hal_resources.h).

## MCP23017 — Buttons & RGB wiring guide
The MCP23017 I/O expander is used for button inputs, the RGB status LED and the vibration motor.

- Default I2C address: 0x20 (7-bit). The driver probes on the "power" I2C bus (I2C1) by default.
- The driver enables internal pull-ups on input pins (GPPUA/GPPUB) and configures interrupt-on-change. Wire buttons in the active-low configuration (button -> MCP pin -> GND).

Known pin assignments (MCP23017 pin indices used by the HAL):

- Port B (pins 8..15):
  - B0 (index 8) — Vibration motor control (use a transistor/MOSFET; do NOT drive motor directly from the MCP pin).
  - B1 (index 9) — RGB Red (digital on/off via MCP functions)
  - B2 (index 10) — RGB Green
  - B3 (index 11) — RGB Blue

- Port A (pins 0..7):
  - Typically used for button inputs (active-low). The specific button-to-pin mapping depends on your board wiring; common wiring connects each button between the MCP pin and GND.

Example wiring recommendations
- Buttons: connect one side of each tactile switch to an MCP23017 GPIO (Port A recommended). Connect the other side to GND. The HAL will enable internal pull-ups so the GPIO reads HIGH when idle and LOW when pressed.
- RGB LED: connect each LED cathode (or anode depending on LED common type) to the MCP23017 pin, and the LED other terminal to the appropriate supply through a current-limiting resistor. For common-anode RGB, connect the anode to 3.3V and use MCP outputs to sink (check your LED type). Keep LED current per color <20 mA; consider using transistors or MOSFETs for higher brightness.
- Vibro motor: drive the motor via an N-channel MOSFET or transistor controlled by MCP23017 B0; add a diode or snubber and a separate power supply if required.

Interrupt wiring
- Tie the MCP23017 INT pin to MCU `PB0` (`MCP_INT_Pin`) so the MCU EXTI line notices button changes. The HAL calls `furi_hal_mcp23017_attach_int()` to install the handler; ensure `PB0` is connected and enabled in the board wiring.

Driver notes
- The HAL configures MCP23017 IOCON with mirror interrupts and open-drain behaviour and writes GPPUA/GPPUB (pull-ups). Buttons should be wired active-low.
- Use `furi_hal_mcp23017_set_i2c_bus()` if you need to switch the MCP to the external I2C bus (`furi_hal_i2c_handle_external`).

Safety
- Do not connect motors or high-current LEDs directly to MCP23017 pins. Use proper driver stages and consider heat/EMI mitigation for motors.

### Button-to-MCP mapping (from firmware)
The running firmware contains a default mapping array `mcp_pin_map_default` in
`applications/services/input/input.c`. The current mapping used by the HAL is:

| Logical key | Input index | MCP pin index | MCP port/pin |
|---:|---:|---:|---|
| Up    | 0 | 0 | GPA0 |
| Down  | 1 | 4 | GPA4 |
| Right | 2 | 1 | GPA1 |
| Left  | 3 | 5 | GPA5 |
| OK    | 4 | 2 | GPA2 |
| Back  | 5 | 3 | GPA3 |

If your physical board uses only five buttons or a different wiring, update
the `applications/services/input/input.c` to match your wiring.

## How to flash (OTP + firmware)
Before you start
- This touches OTP (One-Time Programmable memory). Proceed with caution.
- Keep the PC powered and avoid USB disconnects.
- Install correct drivers; on Windows use Zadig to set USB Serial or WinUSB if needed.

Step 1 — Create OTP file
1. Open your OTP utility (e.g. `qFlipper OTP.exe`) (In the mics folder).
2. Fill fields (example): Version 12 | Firmware 7 | Body 9 | Connection 6
   - Display: `mgg` — Color: black/white/transparent — Region: `en_ru`/`us_ca_au`/`jp`/`world`
   - Name: up to 8 latin/number chars
3. Generate and save the file.

Step 2 — Write OTP (dangerous)
1. Hold `BOOT0` and plug the board into the PC.
2. Open `STM32CubeProgrammer`, choose `USB` connection and click `Connect`.
3. Select the generated OTP file and set Start Address: `0x1FFF7000`.
4. Click `Start Programming` and wait.

If the device disappears: retry drivers/ports or reinstall STM32CubeProgrammer.

Step 3 — Install firmware (qFlipper)
1. Remove microSD to avoid errors.
2. Open `qFlipper` with the board connected.
3. If not detected, use Zadig to install `USB Serial` / `WinUSB`.
4. Use `Install from file` and pick the `.dfu`.

## Notes & tips
- Use MOSFET/transistor drivers for motors and high-current loads. Add flyback diodes.
- Do not drive vibration motors or speakers directly from I/O without a transistor.
- Verify MCP23017 address straps if you have multiple I2C devices.
- If a peripheral is not detected, try switching MCP23017 to the external I2C bus with `furi_hal_mcp23017_set_i2c_bus()`.

## 📖 Additional documentation
- **[SD Card Contents](documentation/SDCardContents.md)** — Where to find SD card files in the repository
- **[Build Tool (fbt)](documentation/fbt.md)** — How to build firmware and resources
- **[Apps on SD Card](documentation/AppsOnSDCard.md)** — External app development (FAPs)
- **[OTA Updates](documentation/OTA.md)** — Firmware update information

## Credits
Thanks to Nucleus Dark for inspiring this project.

## Currently busy with a high-priority production release: baby_v1.0 🧑‍🍼

- This README was generated with the help of Copilot using a guided structure. I’ve reviewed it carefully, but you may still notice the occasional “robotic” sentence 😄  
- My baby needs his father, but I’ll always do my best to support this project. If you run into any issues, please feel free to open one.  
- If you find this project helpful, please consider supporting me and my growing family ❤️  

## ☕ Support this project
If this project helps you, please consider buying me a coffee: <br>

<a href="https://ko-fi.com/lamtran81949" target="_blank">
  <img src="https://storage.ko-fi.com/cdn/kofi3.png?v=3" 
       alt="Buy Me a Coffee at ko-fi.com"
       height="45">
</a>