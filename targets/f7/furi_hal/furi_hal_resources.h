#pragma once

#include <furi.h>
#include <furi_hal_adc.h>
#include <furi_hal_pwm.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Input Related Constants */
#define INPUT_DEBOUNCE_TICKS 4

//const GpioPin gpio_button_sr_latch = {.port = GPIOH, .pin = LL_GPIO_PIN_3};

// Shared SPI/radio routing: PA4=CC1101_CS, PA5=SPI_SCK, PA6=SPI_MISO, PA7=SPI_MOSI

/* Input Keys */
typedef enum {
    InputKeyUp,
    InputKeyDown,
    InputKeyRight,
    InputKeyLeft,
    InputKeyOk,
    InputKeyBack,
    InputKeyMAX, /**< Special value */
} InputKey;

/* Light */
typedef enum {
    LightRed = (1 << 0),
    LightGreen = (1 << 1),
    LightBlue = (1 << 2),
    LightBacklight = (1 << 3),
} Light;

typedef struct {
    const GpioPin* gpio;
    const InputKey key;
    const bool inverted;
    const char* name;
} InputPin;

typedef struct {
    const GpioPin* pin;
    const char* name;
    const FuriHalAdcChannel channel;
    const FuriHalPwmOutputId pwm_output;
    const uint8_t number;
    const bool debug;
} GpioPinRecord;

extern const InputPin input_pins[];
extern const size_t input_pins_count;

extern const GpioPinRecord gpio_pins[];
extern const size_t gpio_pins_count;

extern const GpioPin gpio_swdio;
extern const GpioPin gpio_swclk;

//extern const GpioPin gpio_vibro;
extern const GpioPin gpio_ibutton;

extern const GpioPin gpio_cc1101_g0;
extern const GpioPin gpio_pcf8574_int;
//extern const GpioPin gpio_rf_sw_0;

extern const GpioPin gpio_subghz_cs;
extern const GpioPin gpio_display_cs;
extern const GpioPin gpio_display_rst_n;
extern const GpioPin gpio_display_di;
extern const GpioPin gpio_sdcard_cs;
//extern const GpioPin gpio_sdcard_cd;
extern const GpioPin gpio_nfc_cs;

// Dont need them but keep them to avoid missing symbol errors
extern const GpioPin gpio_button_up;
extern const GpioPin gpio_button_down;
extern const GpioPin gpio_button_right;
extern const GpioPin gpio_button_left;
extern const GpioPin gpio_button_ok;
extern const GpioPin gpio_button_back;

extern const GpioPin gpio_spi_miso;
extern const GpioPin gpio_spi_mosi;
// extern const GpioPin gpio_spi_mosi1;
extern const GpioPin gpio_spi_sck;

extern const GpioPin gpio_ext_pc0;
extern const GpioPin gpio_ext_pc1;
extern const GpioPin gpio_ext_pc3;
extern const GpioPin gpio_ext_pb2;
extern const GpioPin gpio_ext_pb3;
extern const GpioPin gpio_ext_pa4;
extern const GpioPin gpio_ext_pa6;
extern const GpioPin gpio_ext_pa7;

extern const GpioPin gpio_nfc_irq_rfid_pull;
extern const GpioPin gpio_rfid_carrier_out;
extern const GpioPin gpio_rfid_data_in;
extern const GpioPin gpio_rfid_carrier;

extern const GpioPin gpio_infrared_rx;
extern const GpioPin gpio_infrared_tx;

extern const GpioPin gpio_usart_tx;
extern const GpioPin gpio_usart_rx;

extern const GpioPin gpio_i2c_1_sda;
extern const GpioPin gpio_i2c_1_scl;

extern const GpioPin gpio_i2c_3_sda;
extern const GpioPin gpio_i2c_3_scl;

extern const GpioPin gpio_speaker;

// Legacy alias used by nfclegacy external apps.
#define gpio_spi_r_mosi gpio_spi_mosi

//extern const GpioPin gpio_periph_power;

extern const GpioPin gpio_usb_dm;
extern const GpioPin gpio_usb_dp;

// gpio_button_* MCU pin definitions removed: inputs are provided via PCF8574 wiring

// extern const GpioPin gpio_button_IRQ;
// extern const GpioPin gpio_spi_miso_BTN;
// extern const GpioPin gpio_button_sr_latch;

// ADC for battery voltage measurement
// extern const GpioPin gpio_adc_battery_voltage;
// #define ADC_BATTERY_VOLTAGE_GPIO_Port GPIOA
// #define ADC_BATTERY_VOLTAGE_Pin       LL_GPIO_PIN_0

//extern const GpioPin gpio_button_sr_latch;

#define CC1101_CS_GPIO_Port GPIOA
#define CC1101_CS_Pin       LL_GPIO_PIN_4
#define CC1101_G0_GPIO_Port GPIOA
#define CC1101_G0_Pin       LL_GPIO_PIN_1

#define DISPLAY_CS_GPIO_Port       GPIOA
#define DISPLAY_CS_Pin             LL_GPIO_PIN_2
#define DISPLAY_DI_GPIO_Port       GPIOB
#define DISPLAY_DI_Pin             LL_GPIO_PIN_6
#define DISPLAY_RST_GPIO_Port      GPIOB
#define DISPLAY_RST_Pin            LL_GPIO_PIN_7
// Set to 1 for SSD1306 (0.96") or 0 for SH1106 (1.3") OLED modules
#define DISPLAY_CONTROLLER_SSD1306 0
// Set to 1 for SSD1309 (Waveshare 1.54")
#define DISPLAY_CONTROLLER_SSD1309 1
/* IMP-7: Exactly one display controller must be selected.
 * Enabling both causes double-init and conflicting SPI command sequences. */
#if DISPLAY_CONTROLLER_SSD1306 && DISPLAY_CONTROLLER_SSD1309
#error \
    "Cannot enable both DISPLAY_CONTROLLER_SSD1306 and DISPLAY_CONTROLLER_SSD1309 simultaneously. Set exactly one to 1."
#endif
// SSD1306 init variant: 0 = noname, 1 = vcomh0, 2 = alt0
#define DISPLAY_SSD1306_VARIANT 1
// SSD1309 init variant: 0 = noname0 (x_offset=0), 1 = noname2 (x_offset=2)
// With A1 segment remap, x_offset=2 shifts LEFT — use 0 for 1.54" Waveshare
#define DISPLAY_SSD1309_VARIANT 0
// SPI mode for OLED: 0 = mode0 (CPOL=0, CPHA=0), 3 = mode3 (CPOL=1, CPHA=1)
#define DISPLAY_SPI_MODE        0
// Optional signal inversions for troubleshooting
#define DISPLAY_CS_INVERT       0
#define DISPLAY_DC_INVERT       0
#define DISPLAY_RST_INVERT      0
/* HW SPI (0) uses the SPI1 peripheral with furi_hal_spi_acquire/release mutex,
 * guaranteeing exclusive bus ownership during every display byte.  SW SPI (1)
 * bit-bangs PA5/PA7 but those pins are also owned by the SPI1 peripheral; the
 * SPI1 deactivate callback resets them to Analog, which silently corrupts bytes
 * whenever a SubGHz or SD-card transfer preempts the display mid-byte.
 * SW SPI is fundamentally unreliable on a shared bus — keep this 0.
 */
#define DISPLAY_USE_SW_SPI      0

#define IR_RX_GPIO_Port GPIOA
#define IR_RX_Pin       LL_GPIO_PIN_0
#define IR_TX_GPIO_Port GPIOA
#define IR_TX_Pin       LL_GPIO_PIN_8

#define NFC_CS_GPIO_Port GPIOE
#define NFC_CS_Pin       LL_GPIO_PIN_4

/*
Header pins
5V - 5V
A7 - B5
A6 - A6
A4 - A4
B3 - B3
B2 - B2
C3 - A5
GND - GND

3V3 - 3V3
SWC - SWC (PA14)
GND - GND
SWD - SWD (PA13)
PB6 - Display DI (SPI D/C)
PB7 - Display RST
C1 - B4
C0 - A7
iButton - A3
GND - GND
*/
#define PA7_GPIO_Port GPIOB
#define PA7_Pin       LL_GPIO_PIN_5
#define PA6_GPIO_Port GPIOA
#define PA6_Pin       LL_GPIO_PIN_6
#define PA4_GPIO_Port GPIOA
#define PA4_Pin       LL_GPIO_PIN_4
#define PB3_GPIO_Port GPIOB
#define PB3_Pin       LL_GPIO_PIN_3
#define PB2_GPIO_Port GPIOB
#define PB2_Pin       LL_GPIO_PIN_2
#define PC3_GPIO_Port GPIOA
#define PC3_Pin       LL_GPIO_PIN_5
#define PC1_GPIO_Port GPIOB
#define PC1_Pin       LL_GPIO_PIN_4
#define PC0_GPIO_Port GPIOA
#define PC0_Pin       LL_GPIO_PIN_7

#define QUARTZ_32MHZ_IN_GPIO_Port  GPIOC
#define QUARTZ_32MHZ_IN_Pin        LL_GPIO_PIN_14
#define QUARTZ_32MHZ_OUT_GPIO_Port GPIOC
#define QUARTZ_32MHZ_OUT_Pin       LL_GPIO_PIN_15

// #define RFID_OUT_GPIO_Port     GPIOC
// #define RFID_OUT_Pin           LL_GPIO_PIN_0
// #define RFID_PULL_GPIO_Port    GPIOA
// #define RFID_PULL_Pin          LL_GPIO_PIN_2
// #define RFID_RF_IN_GPIO_Port   GPIOC
// #define RFID_RF_IN_Pin         LL_GPIO_PIN_0
// #define RFID_CARRIER_GPIO_Port GPIOC
// #define RFID_CARRIER_Pin       LL_GPIO_PIN_0
#define RF_SW_0_GPIO_Port GPIOB
#define RF_SW_0_Pin       LL_GPIO_PIN_1

#define SD_CD_GPIO_Port GPIOC
#define SD_CD_Pin       LL_GPIO_PIN_0
#define SD_CS_GPIO_Port GPIOA
#define SD_CS_Pin       LL_GPIO_PIN_10

// PCF8574 interrupt default pin
#define PCF8574_INT_GPIO_Port GPIOB
#define PCF8574_INT_Pin       LL_GPIO_PIN_0

#define SPEAKER_GPIO_Port GPIOB
#define SPEAKER_Pin       LL_GPIO_PIN_8

#define VIBRO_GPIO_Port GPIOC
#define VIBRO_Pin       LL_GPIO_PIN_0

#define iBTN_GPIO_Port GPIOA
#define iBTN_Pin       LL_GPIO_PIN_3

#define USART1_TX_Pin  LL_GPIO_PIN_9
#define USART1_TX_Port GPIOA
/* USART1_RX remapped to PB3 (USART1_RX_ALT / AF7, connector label "PB3") to
 * free PA10 for SD_CS.  PA10 = SD_CS conflicts with USART1_RX: even with
 * serial logging disabled the expansion-module IRQ detection configures the
 * RX GPIO as an interrupt input at boot, which prevents SD_CS being driven low.
 * PB3 is a valid USART1_RX alternate-function pin (AF7) and is an unused
 * ext-connector GPIO on this UBYTE board, so routing USART RX there is safe.
 * If a user application also uses gpio_ext_pb3, it must disable expansion-module
 * serial detection first (furi_hal_serial_control_set_expansion_callback(NULL)).
 */
#define USART1_RX_Pin  LL_GPIO_PIN_3
#define USART1_RX_Port GPIOB

#define SPI_MISO_GPIO_Port GPIOA
#define SPI_MISO_Pin       LL_GPIO_PIN_6
#define SPI_MOSI_GPIO_Port GPIOA
#define SPI_MOSI_Pin       LL_GPIO_PIN_7
#define SPI_SCK_GPIO_Port  GPIOA
#define SPI_SCK_Pin        LL_GPIO_PIN_5

// Legacy NFC IRQ aliases required by external ST25R3916-based apps.
// PN532 on this DIY board does not use these pins; it uses I2C1 SCL/SDA only.
#define NFC_IRQ_Pin       PCF8574_INT_Pin
#define NFC_IRQ_GPIO_Port PCF8574_INT_GPIO_Port

#define I2C_1_SCL_Pin       LL_GPIO_PIN_9
#define I2C_1_SCL_GPIO_Port GPIOA
#define I2C_1_SDA_Pin       LL_GPIO_PIN_9
#define I2C_1_SDA_GPIO_Port GPIOB

#define I2C_3_SCL_GPIO_Port GPIOA
#define I2C_3_SCL_Pin       LL_GPIO_PIN_7
/* NOTE: I2C_3_SCL (PA7) shares the same MCU pin as SPI_MOSI (PA7).
 * I2C3 is disabled on this DIY board and must not be used for PN532.
 * PN532 is on I2C1 through furi_hal_i2c_handle_power (SCL=PA9, SDA=PB9).
 */
#define I2C_3_SDA_GPIO_Port GPIOB
#define I2C_3_SDA_Pin       LL_GPIO_PIN_4

void furi_hal_resources_init_early(void);

void furi_hal_resources_deinit_early(void);

void furi_hal_resources_init(void);

/** Get a corresponding external connector pin number for a gpio
 *
 * @param      gpio  GpioPin
 *
 * @return     pin number or -1 if gpio is not on the external connector
 */
int32_t furi_hal_resources_get_ext_pin_number(const GpioPin* gpio);

/**
 * @brief Finds a pin by its name
 * 
 * @param name case-insensitive pin name to look for (e.g. `"Pc3"`, `"pA4"`)
 * 
 * @return a pointer to the corresponding `GpioPinRecord` if such a pin exists,
 *         `NULL` otherwise.
 */
const GpioPinRecord* furi_hal_resources_pin_by_name(const char* name);

/**
 * @brief Finds a pin by its number
 * 
 * @param name pin number to look for (e.g. `7`, `4`)
 * 
 * @return a pointer to the corresponding `GpioPinRecord` if such a pin exists,
 *         `NULL` otherwise.
 */
const GpioPinRecord* furi_hal_resources_pin_by_number(uint8_t number);

#ifdef __cplusplus
}
#endif
