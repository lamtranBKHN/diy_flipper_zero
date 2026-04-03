#include <furi_hal_resources.h>
#include <furi_hal_bus.h>
#include <furi.h>

#include <stm32wbxx_ll_rcc.h>
#include <stm32wbxx_ll_pwr.h>

#define TAG "FuriHalResources"

// Legacy/debug GPIOs removed: keep main pin definitions only.


const GpioPin gpio_swdio = {.port = GPIOA, .pin = LL_GPIO_PIN_13};
const GpioPin gpio_swclk = {.port = GPIOA, .pin = LL_GPIO_PIN_14};

// vibro not used on this board variant
const GpioPin gpio_ibutton = {.port = iBTN_GPIO_Port, .pin = iBTN_Pin};

const GpioPin gpio_cc1101_g0 = {.port = CC1101_G0_GPIO_Port, .pin = CC1101_G0_Pin};
const GpioPin gpio_mcp_int = {.port = MCP_INT_GPIO_Port, .pin = MCP_INT_Pin};
// RF switch pin omitted for this board

const GpioPin gpio_subghz_cs = {.port = CC1101_CS_GPIO_Port, .pin = CC1101_CS_Pin};
// const GpioPin gpio_display_cs = {.port = DISPLAY_CS_GPIO_Port, .pin = DISPLAY_CS_Pin};
// const GpioPin gpio_display_rst_n = {.port = DISPLAY_RST_GPIO_Port, .pin = DISPLAY_RST_Pin};
// const GpioPin gpio_display_di = {.port = DISPLAY_DI_GPIO_Port, .pin = DISPLAY_DI_Pin};
const GpioPin gpio_sdcard_cs = {.port = SD_CS_GPIO_Port, .pin = SD_CS_Pin};
// SD card CD not used
const GpioPin gpio_nfc_cs = {.port = NFC_CS_GPIO_Port, .pin = NFC_CS_Pin};
const GpioPin gpio_nfc_irq_rfid_pull = {.port = NFC_IRQ_GPIO_Port, .pin = NFC_IRQ_Pin};

// MCU button GpioPin definitions removed — board uses MCP23017 for inputs.

const GpioPin gpio_spi_miso = {.port = SPI_MISO_GPIO_Port, .pin = SPI_MISO_Pin};
const GpioPin gpio_spi_mosi = {.port = SPI_MOSI_GPIO_Port, .pin = SPI_MOSI_Pin};
// const GpioPin gpio_spi_mosi1 = {.port = SPI_MOSI_GPIO_Port1, .pin = SPI_MOSI_Pin1};
const GpioPin gpio_spi_sck = {.port = SPI_SCK_GPIO_Port, .pin = SPI_SCK_Pin};

const GpioPin gpio_ext_pc0 = {.port = PC0_GPIO_Port, .pin = PC0_Pin};
const GpioPin gpio_ext_pc1 = {.port = PC1_GPIO_Port, .pin = PC1_Pin};
const GpioPin gpio_ext_pc3 = {.port = PC3_GPIO_Port, .pin = PC3_Pin};
const GpioPin gpio_ext_pb2 = {.port = PB2_GPIO_Port, .pin = PB2_Pin};
const GpioPin gpio_ext_pb3 = {.port = PB3_GPIO_Port, .pin = PB3_Pin};
const GpioPin gpio_ext_pa4 = {.port = PA4_GPIO_Port, .pin = PA4_Pin};
const GpioPin gpio_ext_pa6 = {.port = PA6_GPIO_Port, .pin = PA6_Pin};
const GpioPin gpio_ext_pa7 = {.port = PA7_GPIO_Port, .pin = PA7_Pin};

const GpioPin gpio_button_up;
const GpioPin gpio_button_down;
const GpioPin gpio_button_right;
const GpioPin gpio_button_left;
const GpioPin gpio_button_ok;
const GpioPin gpio_button_back;

const GpioPin gpio_infrared_rx = {.port = IR_RX_GPIO_Port, .pin = IR_RX_Pin};
const GpioPin gpio_infrared_tx = {.port = IR_TX_GPIO_Port, .pin = IR_TX_Pin};

const GpioPin gpio_usart_tx = {.port = USART1_TX_Port, .pin = USART1_TX_Pin};
const GpioPin gpio_usart_rx = {.port = USART1_RX_Port, .pin = USART1_RX_Pin};

const GpioPin gpio_i2c_1_sda = {.port = I2C_1_SDA_GPIO_Port, .pin = I2C_1_SDA_Pin};
const GpioPin gpio_i2c_1_scl = {.port = I2C_1_SCL_GPIO_Port, .pin = I2C_1_SCL_Pin};

const GpioPin gpio_i2c_3_sda = {.port = I2C_3_SDA_GPIO_Port, .pin = I2C_3_SDA_Pin};
const GpioPin gpio_i2c_3_scl = {.port = I2C_3_SCL_GPIO_Port, .pin = I2C_3_SCL_Pin};


const GpioPin gpio_speaker = {.port = SPEAKER_GPIO_Port, .pin = SPEAKER_Pin};

// peripheral power control not present on this board

const GpioPin gpio_usb_dm = {.port = GPIOA, .pin = LL_GPIO_PIN_11};
const GpioPin gpio_usb_dp = {.port = GPIOA, .pin = LL_GPIO_PIN_12};

// const GpioPin gpio_adc_battery_voltage = {.port = ADC_BATTERY_VOLTAGE_GPIO_Port, .pin = ADC_BATTERY_VOLTAGE_Pin};

const GpioPinRecord gpio_pins[] = {
    // 5V: 1
    {.pin = &gpio_ext_pa7,
     .name = "PA7",
     .channel = FuriHalAdcChannel12,
     .pwm_output = FuriHalPwmOutputIdTim1PA7,
     .number = 2,
     .debug = false},
    {.pin = &gpio_ext_pa6,
     .name = "PA6",
     .channel = FuriHalAdcChannel11,
     .number = 3,
     .debug = false},
    {.pin = &gpio_ext_pa4,
     .name = "PA4",
     .channel = FuriHalAdcChannel9,
     .pwm_output = FuriHalPwmOutputIdLptim2PA4,
     .number = 4,
     .debug = false},
    {.pin = &gpio_ext_pb3,
     .name = "PB3",
     .channel = FuriHalAdcChannelNone,
     .number = 5,
     .debug = false},
    {.pin = &gpio_ext_pb2,
     .name = "PB2",
     .channel = FuriHalAdcChannelNone,
     .number = 6,
     .debug = false},
    {.pin = &gpio_ext_pc3,
     .name = "PC3",
     .channel = FuriHalAdcChannel4,
     .number = 7,
     .debug = false},
    // GND: 8
    // Space
    // 3v3: 9
    {.pin = &gpio_swclk,
     .name = "PA14",
     .channel = FuriHalAdcChannelNone,
     .number = 10,
     .debug = true},
    // GND: 11
    {.pin = &gpio_swdio,
     .name = "PA13",
     .channel = FuriHalAdcChannelNone,
     .number = 12,
     .debug = true},
    {.pin = &gpio_usart_tx,
     .name = "PB6",
     .channel = FuriHalAdcChannelNone,
     .number = 13,
     .debug = true},
    {.pin = &gpio_usart_rx,
     .name = "PB7",
     .channel = FuriHalAdcChannelNone,
     .number = 14,
     .debug = true},
    {.pin = &gpio_ext_pc1,
     .name = "PC1",
     .channel = FuriHalAdcChannel2,
     .number = 15,
     .debug = false},
    {.pin = &gpio_ext_pc0,
     .name = "PC0",
     .channel = FuriHalAdcChannel1,
     .number = 16,
     .debug = false},
    {.pin = &gpio_ibutton,
     .name = "PA3",
     .channel = FuriHalAdcChannelNone,
     .number = 17,
     .debug = true},
    // GND: 18

    /* Dangerous pins, may damage hardware */
    {.pin = &gpio_speaker,
     .name = "PB8",
     .channel = FuriHalAdcChannelNone,
     .number = 0,
     .debug = true},
    {.pin = &gpio_infrared_tx,
     .name = "PA8",
     .channel = FuriHalAdcChannelNone,
     .number = 0,
     .debug = true},
};

const size_t gpio_pins_count = COUNT_OF(gpio_pins);

// Old MCU-driven input pin array removed — input is handled via MCP23017 on this board.

const InputPin input_pins[] = {
    {.gpio = NULL, .key = InputKeyUp, .inverted = true, .name = "Up"},
    {.gpio = NULL, .key = InputKeyDown, .inverted = true, .name = "Down"},
    {.gpio = NULL, .key = InputKeyRight, .inverted = true, .name = "Right"},
    {.gpio = NULL, .key = InputKeyLeft, .inverted = true, .name = "Left"},
    {.gpio = NULL, .key = InputKeyOk, .inverted = true, .name = "OK"},
    {.gpio = NULL, .key = InputKeyBack, .inverted = true, .name = "Back"},
};


const size_t input_pins_count = COUNT_OF(input_pins);

// static void furi_hal_resources_init_input_pins(GpioMode mode) {
//     for(size_t i = 0; i < input_pins_count; i++) {
//         if(input_pins[i].gpio != NULL) {
//             furi_hal_gpio_init(
//                 input_pins[i].gpio,
//                 mode,
//                 (input_pins[i].inverted) ? GpioPullUp : GpioPullDown,
//                 GpioSpeedLow);
//         }
//     }
// }

static void furi_hal_resources_init_gpio_pins(GpioMode mode) {
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(!gpio_pins[i].debug) {
            furi_hal_gpio_init(gpio_pins[i].pin, mode, GpioPullNo, GpioSpeedLow);
        }
    }
}

void furi_hal_resources_init_early(void) {
    furi_hal_bus_enable(FuriHalBusGPIOA);
    furi_hal_bus_enable(FuriHalBusGPIOB);
    furi_hal_bus_enable(FuriHalBusGPIOC);
    furi_hal_bus_enable(FuriHalBusGPIOD);
    furi_hal_bus_enable(FuriHalBusGPIOE);
    furi_hal_bus_enable(FuriHalBusGPIOH);

    // furi_hal_resources_init_input_pins(GpioModeInput);

    // Explicit, surviving reset, pulls
    LL_PWR_EnablePUPDCfg();
    // LL_PWR_EnableGPIOPullDown(LL_PWR_GPIO_A, LL_PWR_GPIO_BIT_8); // gpio_vibro
    // LL_PWR_EnableGPIOPullDown(LL_PWR_GPIO_A, LL_PWR_GPIO_BIT_6); // gpio_speaker
    // LL_PWR_EnableGPIOPullDown(LL_PWR_GPIO_B, LL_PWR_GPIO_BIT_9); // gpio_infrared_tx

    // SD Card stepdown control
    // furi_hal_gpio_write(&gpio_periph_power, 1);
    // furi_hal_gpio_init(&gpio_periph_power, GpioModeOutputOpenDrain, GpioPullNo, GpioSpeedLow);

    // Display pins
    // furi_hal_gpio_write(&gpio_display_rst_n, 0);
    // furi_hal_gpio_init_simple(&gpio_display_rst_n, GpioModeOutputPushPull);
    // LL_PWR_EnableGPIOPullUp(LL_PWR_GPIO_B, LL_PWR_GPIO_BIT_0); // gpio_display_rst_n
    // furi_hal_gpio_write(&gpio_display_di, 0);
    // furi_hal_gpio_init_simple(&gpio_display_di, GpioModeOutputPushPull);
    // LL_PWR_EnableGPIOPullDown(LL_PWR_GPIO_B, LL_PWR_GPIO_BIT_1); // gpio_display_di

    // Hard reset USB
    furi_hal_gpio_write(&gpio_usb_dm, 1);
    furi_hal_gpio_write(&gpio_usb_dp, 1);
    furi_hal_gpio_init_simple(&gpio_usb_dm, GpioModeOutputOpenDrain);
    furi_hal_gpio_init_simple(&gpio_usb_dp, GpioModeOutputOpenDrain);
    furi_hal_gpio_write(&gpio_usb_dm, 0);
    furi_hal_gpio_write(&gpio_usb_dp, 0);
    furi_delay_us(5); // Device Driven disconnect: 2.5us + extra to compensate cables
    furi_hal_gpio_write(&gpio_usb_dm, 1);
    furi_hal_gpio_write(&gpio_usb_dp, 1);
    furi_hal_gpio_init_simple(&gpio_usb_dm, GpioModeAnalog);
    furi_hal_gpio_init_simple(&gpio_usb_dp, GpioModeAnalog);
    furi_hal_gpio_write(&gpio_usb_dm, 0);
    furi_hal_gpio_write(&gpio_usb_dp, 0);

    // External header pins
    furi_hal_resources_init_gpio_pins(GpioModeAnalog);
}

void furi_hal_resources_deinit_early(void) {
    furi_hal_bus_disable(FuriHalBusGPIOA);
    furi_hal_bus_disable(FuriHalBusGPIOB);
    furi_hal_bus_disable(FuriHalBusGPIOC);
    furi_hal_bus_disable(FuriHalBusGPIOD);
    furi_hal_bus_disable(FuriHalBusGPIOE);
    furi_hal_bus_disable(FuriHalBusGPIOH);
}

void furi_hal_resources_init(void) {

    
   furi_hal_gpio_init(&gpio_ibutton, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

    // furi_hal_gpio_init(&gpio_nfc_irq_rfid_pull, GpioModeInterruptRiseFall, GpioPullUp, GpioSpeedLow);
    // FURI_LOG_T(TAG, "IRQ4");

  //  furi_hal_gpio_init(&gpio_rf_sw_0, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);

    NVIC_SetPriority(EXTI0_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(EXTI0_IRQn);

    NVIC_SetPriority(EXTI1_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(EXTI1_IRQn);

    NVIC_SetPriority(EXTI2_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(EXTI2_IRQn);

    NVIC_SetPriority(EXTI3_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(EXTI3_IRQn);

    NVIC_SetPriority(EXTI4_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(EXTI4_IRQn);

    // EXTI6 not used in this board configuration (MCP23017 INT line is routed elsewhere)

    NVIC_SetPriority(EXTI9_5_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(EXTI9_5_IRQn);

    NVIC_SetPriority(EXTI15_10_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(EXTI15_10_IRQn);

    FURI_LOG_I(TAG, "Init OK");
}

int32_t furi_hal_resources_get_ext_pin_number(const GpioPin* gpio) {
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(gpio_pins[i].pin == gpio) {
            return gpio_pins[i].number;
        }
    }
    return -1;
}

const GpioPinRecord* furi_hal_resources_pin_by_name(const char* name) {
    for(size_t i = 0; i < gpio_pins_count; i++) {
        const GpioPinRecord* record = &gpio_pins[i];
        if(strcasecmp(name, record->name) == 0) return record;
    }
    return NULL;
}

const GpioPinRecord* furi_hal_resources_pin_by_number(uint8_t number) {
    for(size_t i = 0; i < gpio_pins_count; i++) {
        const GpioPinRecord* record = &gpio_pins[i];
        if(record->number == number) return record;
    }
    return NULL;
}
