// SPDX-License-Identifier: MIT
// Cortex-M4 firmware of lvgl-mpy-linux for the STM32MP157C-DK2: it serves machine.I2C,
// machine.ADC, machine.PWM and machine.SPI to MicroPython on Linux, through the protocol of mpy_m4_protocol.h over an RPMsg
// virtual serial port (/dev/ttyRPMSG0 on Linux).
//
// Linux loads it through remoteproc and, from the "m4-examples" device tree, gives it its
// peripherals: clocks, pins (I2C5 on PA11/PA12, the orange LED on PH7) and the ADC's 2.5 V
// reference. Linux only reserves the pins: the firmware sets them. It never touches the system
// clocks.

#include <string.h>

#include "main.h"
#include "mpy_m4_protocol.h"

#define FIRMWARE_TEXT "mpy-m4 stm32mp157c-dk2"

IPCC_HandleTypeDef hipcc;  // used by ST's mbox_ipcc.c
static VIRT_UART_HandleTypeDef channel;

// Bytes received from Linux: the serial port is a stream, so a request may arrive in pieces.
static uint8_t received[2 * MPY_M4_MAX_FRAME];
static size_t received_count;

static uint8_t answer[MPY_M4_MAX_FRAME + 4];

static I2C_HandleTypeDef i2c;
static uint32_t i2c_frequency;
static ADC_HandleTypeDef adc[2];
static uint8_t adc_ready[2];

void Error_Handler(void) {
    while (1) {
    }
}

static void on_receive(VIRT_UART_HandleTypeDef *huart) {
    size_t size = huart->RxXferSize;
    if (received_count + size > sizeof(received)) {
        received_count = 0;  // nonsense: start again; the requests in it get no answer
        return;
    }
    memcpy(received + received_count, huart->pRxBuffPtr, size);
    received_count += size;
}

static void send(uint8_t sequence, uint8_t status, const uint8_t *data, size_t size) {
    size_t length = 2 + size;
    answer[0] = length & 0xFF;
    answer[1] = length >> 8;
    answer[2] = sequence;
    answer[3] = status;
    if (size) {
        memcpy(answer + 4, data, size);
    }
    VIRT_UART_Transmit(&channel, answer, 2 + length);
}

static uint16_t get16(const uint8_t *p) {
    return p[0] | (p[1] << 8);
}

static uint32_t get32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---- I2C5, on the Arduino connector (D14 SDA = PA12, D15 SCL = PA11) and the 40-pin header

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c) {
    // Linux reserves PA11 and PA12 for the M4 (m4-i2c5 pins) but leaves them as they are: the
    // M4 sets them. The weak internal pull-ups keep an empty bus high, so that an address nobody
    // answers is a NACK (ENODEV), not a stuck bus; real devices bring their own pull-ups.
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef pins = {
        .Pin = GPIO_PIN_11 | GPIO_PIN_12,
        .Mode = GPIO_MODE_AF_OD,
        .Pull = GPIO_PULLUP,
        .Speed = GPIO_SPEED_FREQ_LOW,
        .Alternate = GPIO_AF4_I2C5,
    };
    PERIPH_LOCK(GPIOA);
    HAL_GPIO_Init(GPIOA, &pins);
    PERIPH_UNLOCK(GPIOA);
    __HAL_RCC_I2C5_CLK_ENABLE();
}

// TIMINGR for a bus frequency, from the I2C kernel clock: a prescaled tick, then low and high
// periods of the SCL clock (55% and 45%, less the synchronisation and rise times).
static uint32_t i2c_timing(uint32_t kernel, uint32_t frequency, uint32_t *actual) {
    uint32_t target = frequency <= 100000 ? 4000000 : 16000000;
    uint32_t prescaler = (kernel + target - 1) / target;
    if (prescaler < 1) {
        prescaler = 1;
    } else if (prescaler > 16) {
        prescaler = 16;
    }
    uint32_t tick = kernel / prescaler;
    uint32_t period = tick / frequency;
    uint32_t low = period * 55 / 100;
    uint32_t high = period - low;
    uint32_t sync = 2 + 1;  // two kernel clocks of synchronisation, about a tick of rise time
    high = high > sync + 1 ? high - sync : 1;
    if (low > 256) {
        low = 256;
    }
    if (high > 256) {
        high = 256;
    }
    uint32_t setup = tick / 2000000 + 1;  // data setup >= 250 ns (100 ns in fast mode)
    uint32_t hold = 1;
    if (setup > 16) {
        setup = 16;
    }
    *actual = tick / (low + high + sync);
    return ((prescaler - 1) << 28) | ((setup - 1) << 20) | (hold << 16) | ((high - 1) << 8) | (low - 1);
}

static int i2c_init(uint32_t frequency) {
    if (frequency < 10000 || frequency > 1000000) {
        return E_INVAL;
    }
    if (i2c_frequency) {
        HAL_I2C_DeInit(&i2c);
        i2c_frequency = 0;
    }
    uint32_t kernel = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_I2C35);
    if (!kernel) {
        return E_IO;
    }
    uint32_t actual;
    memset(&i2c, 0, sizeof(i2c));
    i2c.Instance = I2C5;
    i2c.Init.Timing = i2c_timing(kernel, frequency, &actual);
    i2c.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    i2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    i2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    i2c.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&i2c) != HAL_OK) {
        return E_IO;
    }
    HAL_I2CEx_ConfigAnalogFilter(&i2c, I2C_ANALOGFILTER_ENABLE);
    i2c_frequency = actual;
    return 0;
}

static int i2c_ready(void) {
    return i2c_frequency ? 0 : i2c_init(100000);
}

// The errno of a failed transfer; sent is true once the address was acknowledged.
static int i2c_error(HAL_StatusTypeDef status, int sent) {
    uint32_t error = HAL_I2C_GetError(&i2c);
    if (error & HAL_I2C_ERROR_AF) {
        return sent ? E_IO : E_NODEV;
    }
    if (status == HAL_TIMEOUT || (error & HAL_I2C_ERROR_TIMEOUT)) {
        return E_TIMEDOUT;
    }
    return E_IO;
}

static uint32_t i2c_timeout(size_t bytes) {
    // Ten bit times a byte at the slowest bus, and room for clock stretching.
    return 10 + bytes / 4;
}

static void handle_i2c(uint8_t sequence, uint8_t operation, const uint8_t *args, size_t size) {
    static uint8_t data[MPY_M4_MAX_DATA];
    int error = operation == OP_I2C_INIT ? 0 : i2c_ready();
    if (error) {
        send(sequence, error, NULL, 0);
        return;
    }
    HAL_StatusTypeDef status;
    switch (operation) {
        case OP_I2C_INIT: {
            if (size != 4) {
                break;
            }
            error = i2c_init(get32(args));
            uint8_t result[4] = {i2c_frequency, i2c_frequency >> 8, i2c_frequency >> 16, i2c_frequency >> 24};
            send(sequence, error, result, error ? 0 : 4);
            return;
        }
        case OP_I2C_SCAN: {
            size_t found = 0;
            for (uint16_t address = 0x08; address < 0x78; ++address) {
                if (HAL_I2C_IsDeviceReady(&i2c, address << 1, 1, 2) == HAL_OK) {
                    data[found++] = address;
                }
            }
            send(sequence, 0, data, found);
            return;
        }
        case OP_I2C_WRITE: {
            if (size < 1 || size - 1 > MPY_M4_MAX_DATA) {
                break;
            }
            uint16_t address = args[0] << 1, count = size - 1;
            if (count == 0) {
                status = HAL_I2C_IsDeviceReady(&i2c, address, 1, 2);
            } else {
                status = HAL_I2C_Master_Transmit(&i2c, address, (uint8_t *)args + 1, count, i2c_timeout(count));
            }
            if (status != HAL_OK) {
                // The bytes before a refused one count as acknowledged, as machine.I2C counts.
                uint16_t left = i2c.XferCount;
                if ((HAL_I2C_GetError(&i2c) & HAL_I2C_ERROR_AF) && count && left < count) {
                    uint16_t acked = count - left - 1;
                    uint8_t result[2] = {acked, acked >> 8};
                    send(sequence, 0, result, 2);
                } else {
                    send(sequence, i2c_error(status, 0), NULL, 0);
                }
                return;
            }
            uint8_t result[2] = {count, count >> 8};
            send(sequence, 0, result, 2);
            return;
        }
        case OP_I2C_READ: {
            if (size != 3) {
                break;
            }
            uint16_t count = get16(args + 1);
            if (count == 0 || count > MPY_M4_MAX_DATA) {
                break;
            }
            status = HAL_I2C_Master_Receive(&i2c, args[0] << 1, data, count, i2c_timeout(count));
            if (status != HAL_OK) {
                send(sequence, i2c_error(status, 0), NULL, 0);
                return;
            }
            send(sequence, 0, data, count);
            return;
        }
        case OP_I2C_MEM_READ:
        case OP_I2C_MEM_WRITE: {
            if (size < 4 || (args[1] != 1 && args[1] != 2)) {
                break;
            }
            uint16_t address = args[0] << 1, memory = get16(args + 2);
            uint16_t width = args[1] == 1 ? I2C_MEMADD_SIZE_8BIT : I2C_MEMADD_SIZE_16BIT;
            if (operation == OP_I2C_MEM_READ) {
                if (size != 6) {
                    break;
                }
                uint16_t count = get16(args + 4);
                if (count == 0 || count > MPY_M4_MAX_DATA) {
                    break;
                }
                status = HAL_I2C_Mem_Read(&i2c, address, memory, width, data, count, i2c_timeout(count));
                if (status != HAL_OK) {
                    send(sequence, i2c_error(status, 0), NULL, 0);
                    return;
                }
                send(sequence, 0, data, count);
            } else {
                uint16_t count = size - 4;
                if (count > MPY_M4_MAX_DATA) {
                    break;
                }
                status = HAL_I2C_Mem_Write(&i2c, address, memory, width, (uint8_t *)args + 4, count,
                    i2c_timeout(count));
                send(sequence, status == HAL_OK ? 0 : i2c_error(status, 0), NULL, 0);
            }
            return;
        }
    }
    send(sequence, E_INVAL, NULL, 0);
}

// ---- ADC1 and ADC2, with the 2.5 V reference buffer (VREFBUF) that Linux turns on for the M4

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc) {
    __HAL_RCC_ADC12_CLK_ENABLE();
}

static int adc_ready_for(int number) {
    ADC_HandleTypeDef *h = &adc[number - 1];
    if (adc_ready[number - 1]) {
        return 0;
    }
    if (!adc_ready[0] && !adc_ready[1]) {
        // The boot loader uses ADC1 (USB-C power detection) and leaves it configured and on;
        // the HAL then cannot connect the internal channels and ADC1 does not convert. A reset of
        // the ADC block starts both from scratch; the device tree gives both to the M4.
        __HAL_RCC_ADC12_CLK_ENABLE();
        __HAL_RCC_ADC12_FORCE_RESET();
        __HAL_RCC_ADC12_RELEASE_RESET();
    }
    memset(h, 0, sizeof(*h));
    h->Instance = number == 1 ? ADC1 : ADC2;
    h->Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV2;  // the 24 MHz kernel clock, under the 36 MHz limit
    h->Init.Resolution = ADC_RESOLUTION_16B;
    h->Init.ScanConvMode = ADC_SCAN_DISABLE;
    h->Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    h->Init.LowPowerAutoWait = DISABLE;
    h->Init.ContinuousConvMode = DISABLE;
    h->Init.NbrOfConversion = 1;
    h->Init.DiscontinuousConvMode = DISABLE;
    h->Init.ExternalTrigConv = ADC_SOFTWARE_START;
    h->Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    h->Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    h->Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    h->Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
    h->Init.OversamplingMode = DISABLE;
    if (HAL_ADC_Init(h) != HAL_OK ||
        HAL_ADCEx_Calibration_Start(h, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED) != HAL_OK) {
        return E_IO;
    }
    // The calibration leaves the ADC on, and the HAL connects the internal channels (VREFINT,
    // temperature) only while both ADCs are off: off until each conversion.
    HAL_ADC_Stop(h);
    adc_ready[number - 1] = 1;
    return 0;
}

static void analog_pin(uint8_t port, uint8_t pin) {
    // Ports A to K; their clocks are bits 0 to 10 of the M4's AHB4 enable register.
    GPIO_TypeDef *gpio = (GPIO_TypeDef *)(GPIOA_BASE + port * (GPIOB_BASE - GPIOA_BASE));
    SET_BIT(RCC->MC_AHB4ENSETR, 1U << port);
    GPIO_InitTypeDef init = {.Pin = 1U << pin, .Mode = GPIO_MODE_ANALOG, .Pull = GPIO_NOPULL};
    PERIPH_LOCK(gpio);
    HAL_GPIO_Init(gpio, &init);
    PERIPH_UNLOCK(gpio);
}

static void handle_adc(uint8_t sequence, const uint8_t *args, size_t size) {
    if (size != 4 || (args[0] != 1 && args[0] != 2)) {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    int number = args[0];
    uint32_t channel;
    if (args[1] < 20) {
        static const uint32_t inputs[20] = {ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
            ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7, ADC_CHANNEL_8, ADC_CHANNEL_9,
            ADC_CHANNEL_10, ADC_CHANNEL_11, ADC_CHANNEL_12, ADC_CHANNEL_13, ADC_CHANNEL_14, ADC_CHANNEL_15,
            ADC_CHANNEL_16, ADC_CHANNEL_17, ADC_CHANNEL_18, ADC_CHANNEL_19};
        channel = inputs[args[1]];
    } else if (args[1] == ADC_CHANNEL_CODE_VREFINT && number == 2) {
        channel = ADC_CHANNEL_VREFINT;
    } else if (args[1] == ADC_CHANNEL_CODE_TEMPSENSOR && number == 2) {
        channel = ADC_CHANNEL_TEMPSENSOR;
    } else {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    if (args[2] != ADC_NO_PIN) {
        if (args[2] > 10 || args[3] > 15) {
            send(sequence, E_INVAL, NULL, 0);
            return;
        }
        analog_pin(args[2], args[3]);
    }
    int error = adc_ready_for(number);
    if (error) {
        send(sequence, error, NULL, 0);
        return;
    }
    ADC_HandleTypeDef *h = &adc[number - 1];
    ADC_ChannelConfTypeDef config = {0};
    config.Channel = channel;
    config.Rank = ADC_REGULAR_RANK_1;
    config.SamplingTime = ADC_SAMPLETIME_810CYCLES_5;  // long enough for the internal channels
    config.SingleDiff = ADC_SINGLE_ENDED;
    config.OffsetNumber = ADC_OFFSET_NONE;
    if (HAL_ADC_ConfigChannel(h, &config) != HAL_OK) {
        send(sequence, E_IO, NULL, 0);
        return;
    }
    if (args[1] == ADC_CHANNEL_CODE_TEMPSENSOR) {
        HAL_Delay(1);  // the sensor's start-up time, after ConfigChannel turned it on
    }
    if (HAL_ADC_Start(h) != HAL_OK || HAL_ADC_PollForConversion(h, 10) != HAL_OK) {
        HAL_ADC_Stop(h);
        send(sequence, E_TIMEDOUT, NULL, 0);
        return;
    }
    uint32_t value = HAL_ADC_GetValue(h);
    HAL_ADC_Stop(h);
    uint8_t result[2] = {value, value >> 8};
    send(sequence, 0, result, 2);
}

// ---- PWM on TIM1 channel 1, PE9 (Arduino D6), on the registers

static uint32_t pwm_period;  // timer ticks a period: ARR + 1

static uint32_t timer_clock(void) {
    uint32_t hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_TIMG2);  // TIM1 is in timer group 2
    return hz ? hz : 208877930;
}

static void pwm_pin(int on) {
    __HAL_RCC_GPIOE_CLK_ENABLE();
    GPIO_InitTypeDef pin = {
        .Pin = GPIO_PIN_9,
        .Mode = on ? GPIO_MODE_AF_PP : GPIO_MODE_ANALOG,
        .Pull = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_HIGH,
        .Alternate = on ? GPIO_AF1_TIM1 : 0,
    };
    PERIPH_LOCK(GPIOE);
    HAL_GPIO_Init(GPIOE, &pin);
    PERIPH_UNLOCK(GPIOE);
}

static void handle_pwm(uint8_t sequence, uint8_t operation, const uint8_t *args, size_t size) {
    switch (operation) {
        case OP_PWM_INIT: {
            if (size != 6) {
                break;
            }
            uint32_t clock = timer_clock(), frequency = get32(args), duty = get16(args + 4);
            if (frequency == 0 || frequency > clock / 2) {
                break;
            }
            uint32_t ticks = clock / frequency;  // at most the clock: 32 bits are enough
            uint32_t prescaler = (ticks + 65535) / 65536;  // TIM1 counts to 65535
            uint32_t period = (clock / prescaler + frequency / 2) / frequency;
            __HAL_RCC_TIM1_CLK_ENABLE();
            TIM1->CR1 = 0;
            TIM1->PSC = prescaler - 1;
            TIM1->ARR = period - 1;
            TIM1->CCR1 = (uint64_t)period * duty / 65535;
            TIM1->CCMR1 = (6U << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;  // PWM mode 1, preload
            TIM1->CCER = TIM_CCER_CC1E;
            TIM1->BDTR = TIM_BDTR_MOE;  // an advanced timer: outputs on
            TIM1->EGR = TIM_EGR_UG;
            TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
            pwm_pin(1);
            pwm_period = period;
            uint32_t tick = clock / prescaler, actual = (tick + period / 2) / period;
            uint8_t result[12] = {actual, actual >> 8, actual >> 16, actual >> 24, period, period >> 8,
                period >> 16, period >> 24, tick, tick >> 8, tick >> 16, tick >> 24};
            send(sequence, 0, result, sizeof(result));
            return;
        }
        case OP_PWM_COMPARE: {
            if (size != 4 || !pwm_period || get32(args) > pwm_period) {
                break;
            }
            TIM1->CCR1 = get32(args);  // from the next period (preload): no glitch
            send(sequence, 0, NULL, 0);
            return;
        }
        case OP_PWM_DEINIT:
            if (pwm_period) {
                TIM1->CR1 = 0;
                TIM1->CCER = 0;
                pwm_pin(0);
                __HAL_RCC_TIM1_CLK_DISABLE();
                pwm_period = 0;
            }
            send(sequence, 0, NULL, 0);
            return;
    }
    send(sequence, E_INVAL, NULL, 0);
}

// ---- SPI4 on the registers (the STM32H7 generation of SPI: FIFO, transfer size, CSTART)

static uint8_t spi_mode = 0xFF;  // bit 0 CPOL, bit 1 CPHA, bit 2 LSB first; 0xFF: off

static void spi_pins(int on, uint8_t miso_pull) {
    __HAL_RCC_GPIOE_CLK_ENABLE();
    GPIO_InitTypeDef pins = {
        .Pin = GPIO_PIN_12 | GPIO_PIN_14,
        .Mode = on ? GPIO_MODE_AF_PP : GPIO_MODE_ANALOG,
        .Pull = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_VERY_HIGH,
        .Alternate = on ? GPIO_AF5_SPI4 : 0,
    };
    PERIPH_LOCK(GPIOE);
    HAL_GPIO_Init(GPIOE, &pins);
    pins.Pin = GPIO_PIN_13;
    pins.Pull = !on ? GPIO_NOPULL : miso_pull == 1 ? GPIO_PULLUP : miso_pull == 2 ? GPIO_PULLDOWN : GPIO_NOPULL;
    HAL_GPIO_Init(GPIOE, &pins);
    PERIPH_UNLOCK(GPIOE);
}

static void handle_spi_init(uint8_t sequence, const uint8_t *args, size_t size) {
    if (size != 6 || args[4] > 7 || args[5] > 2) {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    uint32_t clock = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI45);
    if (!clock) {
        clock = 64000000;
    }
    // The fastest rate not above the one asked: the kernel clock over 2, 4, ... 256.
    uint32_t divider = 0, baud = get32(args);
    while (divider < 7 && clock >> (divider + 1) > baud) {
        divider++;
    }
    uint8_t mode = args[4];
    __HAL_RCC_SPI4_CLK_ENABLE();
    // SSI before MASTER: with the internal chip select seen active, a master latches a mode
    // fault (MODF) and turns itself off.
    SPI4->CR1 = 0;
    SPI4->CR1 = SPI_CR1_SSI;
    SPI4->CFG1 = (divider << SPI_CFG1_MBR_Pos) | (7U << SPI_CFG1_DSIZE_Pos);  // 8-bit frames
    // Master, chip select by software, the pins kept driven between transfers (AFCNTR), so
    // that SCK rests at its idle level.
    SPI4->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_AFCNTR | ((mode & 1) ? SPI_CFG2_CPOL : 0) |
        ((mode & 2) ? SPI_CFG2_CPHA : 0) | ((mode & 4) ? SPI_CFG2_LSBFRST : 0);
    SPI4->IFCR = 0xFF8;  // every flag, MODF too
    spi_pins(1, args[5]);
    spi_mode = mode;
    uint32_t actual = clock >> (divider + 1);
    uint8_t result[4] = {actual, actual >> 8, actual >> 16, actual >> 24};
    send(sequence, 0, result, 4);
}

static void handle_spi_transfer(uint8_t sequence, const uint8_t *args, size_t size) {
    static uint8_t data[2 * MPY_M4_MAX_DATA + 4];
    if (spi_mode == 0xFF || size < 2 || size - 1 > MPY_M4_MAX_DATA) {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    uint8_t flags = args[0];
    const uint8_t *out = args + 1;
    size_t count = size - 1, sent = 0, got = 0;
    uint8_t *in = data, *seen = data + count;
    // Watching the pins: SCK is PE12, MOSI PE14. The data are sampled on the leading edge
    // (CPHA 0) or the trailing edge (CPHA 1); the leading edge leaves the idle level (CPOL).
    uint32_t idle = (spi_mode & 1) ? GPIO_PIN_12 : 0;
    uint32_t sample_level = (spi_mode & 2) ? idle : (idle ^ GPIO_PIN_12);
    uint32_t last = GPIOE->IDR & GPIO_PIN_12, edges = 0, bits = 0;
    if (flags & 2) {
        memset(seen, 0, count);
    }

    SPI4->IFCR = 0xFF8;
    SPI4->CR2 = count;
    SPI4->CR1 = SPI_CR1_SSI | SPI_CR1_SPE;
    SPI4->CR1 = SPI_CR1_SSI | SPI_CR1_SPE | SPI_CR1_CSTART;
    uint32_t start = HAL_GetTick();
    while (got < count) {
        uint32_t status = SPI4->SR;
        if (sent < count && (status & SPI_SR_TXP)) {
            *(volatile uint8_t *)&SPI4->TXDR = out[sent++];
        }
        if (status & SPI_SR_RXP) {
            in[got++] = *(volatile uint8_t *)&SPI4->RXDR;
        }
        if (flags & 2) {
            uint32_t pins = GPIOE->IDR;
            uint32_t sck = pins & GPIO_PIN_12;
            if (sck != last) {
                edges++;
                if (sck == sample_level && bits < 8 * count) {
                    uint32_t bit = (pins & GPIO_PIN_14) ? 1 : 0, byte = bits / 8, place = bits % 8;
                    seen[byte] |= bit << ((spi_mode & 4) ? place : 7 - place);
                    bits++;
                }
                last = sck;
            }
        }
        if (HAL_GetTick() - start > 100 + count) {
            break;
        }
    }
    uint32_t wait = HAL_GetTick();
    while (!(SPI4->SR & SPI_SR_EOT) && HAL_GetTick() - wait < 10) {
    }
    if (flags & 2) {  // the last edge, back to idle, comes with the end of transfer
        uint32_t sck = GPIOE->IDR & GPIO_PIN_12;
        if (sck != last) {
            edges++;
        }
    }
    SPI4->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;
    SPI4->CR1 = SPI_CR1_SSI;
    if (got < count) {
        send(sequence, (SPI4->SR & SPI_SR_MODF) ? E_IO : E_TIMEDOUT, NULL, 0);
        return;
    }
    size_t length = 0;
    if (flags & 1) {
        length = count;
    }
    if (flags & 2) {
        if (length != count) {
            memmove(data, seen, count);
        }
        length += count;
        memcpy(data + length, &edges, 4);
        length += 4;
    }
    send(sequence, 0, data, length);
}

static void handle_spi(uint8_t sequence, uint8_t operation, const uint8_t *args, size_t size) {
    switch (operation) {
        case OP_SPI_INIT:
            handle_spi_init(sequence, args, size);
            return;
        case OP_SPI_TRANSFER:
            handle_spi_transfer(sequence, args, size);
            return;
        default:  // OP_SPI_DEINIT
            if (spi_mode != 0xFF) {
                SPI4->CR1 = 0;
                spi_pins(0, 0);
                __HAL_RCC_SPI4_CLK_DISABLE();
                spi_mode = 0xFF;
            }
            send(sequence, 0, NULL, 0);
            return;
    }
}

// ---- a pin sampled for a while, timed by the cycle counter

static void handle_measure(uint8_t sequence, const uint8_t *args, size_t size) {
    if (size != 4 || args[0] > 10 || args[1] > 15 || get16(args + 2) == 0 || get16(args + 2) > 1000) {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    GPIO_TypeDef *gpio = (GPIO_TypeDef *)(GPIOA_BASE + args[0] * (GPIOB_BASE - GPIOA_BASE));
    SET_BIT(RCC->MC_AHB4ENSETR, 1U << args[0]);
    uint32_t mask = 1U << args[1];
    uint32_t core = HAL_RCC_GetMCUFreq();
    uint32_t length = core / 1000 * get16(args + 2);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    // The time between two samples counts as high when the first was high: a loop that takes
    // longer on one branch would bias a count of samples.
    uint32_t samples = 0, high = 0, rising = 0, last = gpio->IDR & mask;
    uint32_t start = DWT->CYCCNT, before = start, now;
    do {
        uint32_t level = gpio->IDR & mask;
        now = DWT->CYCCNT;
        samples++;
        if (last) {
            high += now - before;
        } else if (level) {
            rising++;
        }
        last = level;
        before = now;
    } while (now - start < length);
    uint32_t cycles = now - start;
    uint32_t values[5] = {samples, high, rising, cycles, core};
    send(sequence, 0, (const uint8_t *)values, sizeof(values));  // the M4 is little endian too
}

// ---- requests

static void handle(uint8_t sequence, uint8_t operation, const uint8_t *args, size_t size) {
    switch (operation) {
        case OP_INFO: {
            static const uint8_t info[] = "\x01" FIRMWARE_TEXT;  // MPY_M4_PROTOCOL_VERSION, text
            _Static_assert(MPY_M4_PROTOCOL_VERSION == 1, "the version byte of info");
            send(sequence, 0, info, sizeof(info) - 1);
            return;
        }
        case OP_LED:
            if (size != 1) {
                break;
            }
            if (args[0]) {
                BSP_LED_On(LED7);
            } else {
                BSP_LED_Off(LED7);
            }
            send(sequence, 0, NULL, 0);
            return;
        case OP_I2C_INIT:
        case OP_I2C_SCAN:
        case OP_I2C_WRITE:
        case OP_I2C_READ:
        case OP_I2C_MEM_READ:
        case OP_I2C_MEM_WRITE:
            handle_i2c(sequence, operation, args, size);
            return;
        case OP_ADC_READ:
            handle_adc(sequence, args, size);
            return;
        case OP_PWM_INIT:
        case OP_PWM_COMPARE:
        case OP_PWM_DEINIT:
            handle_pwm(sequence, operation, args, size);
            return;
        case OP_SPI_INIT:
        case OP_SPI_TRANSFER:
        case OP_SPI_DEINIT:
            handle_spi(sequence, operation, args, size);
            return;
        case OP_PIN_MEASURE:
            handle_measure(sequence, args, size);
            return;
        default:
            send(sequence, E_OPNOTSUPP, NULL, 0);
            return;
    }
    send(sequence, E_INVAL, NULL, 0);
}

static void handle_received(void) {
    while (received_count >= 2) {
        size_t length = get16(received);
        if (length < 2 || length > MPY_M4_MAX_FRAME) {
            received_count = 0;  // out of step: drop everything; Linux times out and goes on
            return;
        }
        if (received_count < 2 + length) {
            return;
        }
        handle(received[2], received[3], received + 4, length - 2);
        received_count -= 2 + length;
        memmove(received, received + 2 + length, received_count);
    }
}

int main(void) {
    HAL_Init();
    __HAL_RCC_HSEM_CLK_ENABLE();
    BSP_LED_Init(LED7);
    BSP_LED_Off(LED7);

    hipcc.Instance = IPCC;
    if (HAL_IPCC_Init(&hipcc) != HAL_OK) {
        Error_Handler();
    }
    MX_OPENAMP_Init(RPMSG_REMOTE, NULL);
    if (VIRT_UART_Init(&channel) != VIRT_UART_OK ||
        VIRT_UART_RegisterCallback(&channel, VIRT_UART_RXCPLT_CB_ID, on_receive) != VIRT_UART_OK) {
        Error_Handler();
    }

    while (1) {
        OPENAMP_check_for_message();
        handle_received();
    }
}
