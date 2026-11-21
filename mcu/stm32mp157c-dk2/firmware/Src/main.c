// SPDX-License-Identifier: MIT
// Cortex-M4 firmware of copper-and-code-linux-boards for the STM32MP157C-DK2: it serves machine.I2C, ADC,
// PWM, SPI, UART and Timer to MicroPython on Linux, through the protocol of mpy_m4_protocol.h over
// RPMsg (a raw channel, /dev/rpmsgN on Linux, and a virtual serial port, /dev/ttyRPMSGN).
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

// Two ways in from Linux, the same frames on both: the RPMsg virtual serial port (rpmsg-tty,
// /dev/ttyRPMSG*), and a raw RPMsg channel (rpmsg-raw, /dev/rpmsg* through rpmsg_char), which
// goes around Linux's tty layer. An answer goes back the way its request came; events go the way
// of the last request.
enum { CHANNEL_TTY, CHANNEL_RAW };
static VIRT_UART_HandleTypeDef channel;
static struct rpmsg_endpoint raw;
static int channel_now = CHANNEL_TTY;

// Bytes received: the serial port is a stream, so a request may arrive in pieces.
typedef struct {
    uint8_t data[2 * MPY_M4_MAX_FRAME];
    size_t count;
} inbox_t;
static inbox_t received, raw_received;

static uint8_t answer[MPY_M4_MAX_FRAME + 4];

static I2C_HandleTypeDef i2c;
static uint32_t i2c_frequency;
static ADC_HandleTypeDef adc[2];
static uint8_t adc_ready[2];

void Error_Handler(void) {
    while (1) {
    }
}

static void keep(inbox_t *inbox, const void *data, size_t size) {
    if (inbox->count + size > sizeof(inbox->data)) {
        inbox->count = 0;  // nonsense: start again; the requests in it get no answer
        return;
    }
    memcpy(inbox->data + inbox->count, data, size);
    inbox->count += size;
}

static void on_receive(VIRT_UART_HandleTypeDef *huart) {
    keep(&received, huart->pRxBuffPtr, huart->RxXferSize);
}

static int on_raw(struct rpmsg_endpoint *ept, void *data, size_t size, uint32_t source, void *priv) {
    keep(&raw_received, data, size);
    return 0;
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
    if (channel_now == CHANNEL_RAW) {
        OPENAMP_send(&raw, answer, 2 + length);
    } else {
        VIRT_UART_Transmit(&channel, answer, 2 + length);
    }
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

static uint32_t pwm_tick;  // timer ticks a second

// TIM1 channel 1 at a frequency and a duty (65535: always high), from the first period.
static int pwm_start(uint32_t frequency, uint32_t duty) {
    uint32_t clock = timer_clock();
    if (frequency == 0 || frequency > clock / 2) {
        return 0;
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
    if (!pwm_period) {
        pwm_pin(1);
    }
    pwm_period = period;
    pwm_tick = clock / prescaler;
    return 1;
}

// ---- a melody on the PWM: notes (Hz, ms; 0 Hz a rest) played by the main loop on time

#define MELODY_MAX 100
static uint16_t melody_hz[MELODY_MAX], melody_ms[MELODY_MAX];
static int melody_count, melody_index = -1;  // -1: nothing playing
static uint32_t melody_end;

static void melody_note(void) {
    uint16_t hz = melody_hz[melody_index];
    if (hz) {
        pwm_start(hz, 32768);  // a square wave: half the period high
    } else if (pwm_period) {
        TIM1->CCR1 = 0;  // a rest: the output low, the timer on
    }
    melody_end = HAL_GetTick() + melody_ms[melody_index];
}

static void melody_poll(void) {
    if (melody_index < 0 || (int32_t)(HAL_GetTick() - melody_end) < 0) {
        return;
    }
    if (++melody_index >= melody_count) {
        melody_index = -1;
        if (pwm_period) {
            TIM1->CCR1 = 0;
        }
        return;
    }
    melody_note();
}

static void handle_pwm(uint8_t sequence, uint8_t operation, const uint8_t *args, size_t size) {
    switch (operation) {
        case OP_PWM_INIT: {
            if (size != 6) {
                break;
            }
            melody_index = -1;
            if (!pwm_start(get32(args), get16(args + 4))) {
                break;
            }
            uint32_t period = pwm_period, tick = pwm_tick, actual = (tick + period / 2) / period;
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
        case OP_TONE_SEQUENCE: {
            if (size % 4 || size / 4 > MELODY_MAX) {
                break;
            }
            melody_index = -1;
            melody_count = size / 4;
            for (int i = 0; i < melody_count; ++i) {
                melody_hz[i] = get16(args + 4 * i);
                melody_ms[i] = get16(args + 4 * i + 2);
            }
            if (melody_count) {
                melody_index = 0;
                melody_note();
            }
            send(sequence, 0, NULL, 0);
            return;
        }
        case OP_TONE_STATUS: {
            int16_t index = melody_index;
            uint8_t result[4] = {index, index >> 8, melody_count, melody_count >> 8};
            send(sequence, 0, result, 4);
            return;
        }
        case OP_PWM_DEINIT:
            melody_index = -1;
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

// ---- UART7 on the registers, under interrupt

#define UART_RX_SIZE 4096
#define UART_TX_SIZE 2048
static volatile uint16_t uart_rx[UART_RX_SIZE];  // 16 bits: 9-bit frames
static volatile uint32_t uart_rx_in, uart_rx_out;
static volatile uint16_t uart_tx[UART_TX_SIZE];
static volatile uint32_t uart_tx_in, uart_tx_out;
static volatile uint16_t uart_overruns, uart_framing, uart_parity;
static uint16_t uart_mask;  // the data bits of a frame, without the parity bit
static uint8_t uart_on, uart_wide;  // wide: 9 data bits, two bytes a character on the wire

void UART7_IRQHandler(void) {
    uint32_t status = UART7->ISR;
    if (status & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_PE | USART_ISR_NE)) {
        if (status & USART_ISR_ORE) {
            uart_overruns++;
        }
        if (status & USART_ISR_FE) {
            uart_framing++;
        }
        if (status & USART_ISR_PE) {
            uart_parity++;
        }
        UART7->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_PECF | USART_ICR_NECF;
    }
    while (UART7->ISR & USART_ISR_RXNE_RXFNE) {
        uint16_t value = UART7->RDR & uart_mask;
        uint32_t next = (uart_rx_in + 1) % UART_RX_SIZE;
        if (next == uart_rx_out) {
            uart_overruns++;  // the buffer is full: the newest byte is lost, as on an MCU
        } else {
            uart_rx[uart_rx_in] = value;
            uart_rx_in = next;
        }
    }
    if (UART7->CR1 & USART_CR1_TXEIE_TXFNFIE) {
        while ((UART7->ISR & USART_ISR_TXE_TXFNF) && uart_tx_out != uart_tx_in) {
            UART7->TDR = uart_tx[uart_tx_out];
            uart_tx_out = (uart_tx_out + 1) % UART_TX_SIZE;
        }
        if (uart_tx_out == uart_tx_in) {
            UART7->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
        }
    }
}

static void uart_pins(int on) {
    __HAL_RCC_GPIOE_CLK_ENABLE();
    GPIO_InitTypeDef pins = {
        .Pin = GPIO_PIN_7 | GPIO_PIN_8,
        .Mode = on ? GPIO_MODE_AF_PP : GPIO_MODE_ANALOG,
        .Pull = on ? GPIO_PULLUP : GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_LOW,
        .Alternate = on ? GPIO_AF7_UART7 : 0,
    };
    PERIPH_LOCK(GPIOE);
    HAL_GPIO_Init(GPIOE, &pins);
    PERIPH_UNLOCK(GPIOE);
}

static void uart_off(void) {
    if (uart_on) {
        HAL_NVIC_DisableIRQ(UART7_IRQn);
        UART7->CR1 = 0;
        uart_pins(0);
        __HAL_RCC_UART7_CLK_DISABLE();
        uart_on = 0;
    }
}

static void handle_uart_init(uint8_t sequence, const uint8_t *args, size_t size) {
    if (size != 8) {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    uint32_t baud = get32(args);
    uint8_t bits = args[4], parity = args[5], stop = args[6], flags = args[7];
    // The frame length M counts the parity bit: 7, 8 or 9 bits in all.
    uint32_t frame = bits + (parity ? 1 : 0);
    if (baud == 0 || bits < 7 || frame > 9 || parity > 2 || (stop != 1 && stop != 2) || flags > 1) {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    uint32_t clock = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_UART78);
    if (!clock) {
        clock = 64000000;
    }
    // Oversampling by 16: BRR = clock / baud, 16 to 65535; a prescaler for slow rates.
    static const uint16_t prescalers[12] = {1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256};
    uint32_t index = 0, divider;
    while ((divider = (clock / prescalers[index] + baud / 2) / baud) > 65535 && index < 11) {
        index++;
    }
    if (divider < 16 || divider > 65535) {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    uart_off();
    __HAL_RCC_UART7_CLK_ENABLE();
    UART7->CR1 = 0;
    UART7->PRESC = index;
    UART7->BRR = divider;
    UART7->CR2 = stop == 2 ? USART_CR2_STOP_1 : 0;
    UART7->CR3 = USART_CR3_EIE | ((flags & 1) ? USART_CR3_HDSEL : 0);
    uart_mask = (1U << bits) - 1;
    uart_wide = bits == 9;
    uart_rx_in = uart_rx_out = uart_tx_in = uart_tx_out = 0;
    uart_overruns = uart_framing = uart_parity = 0;
    uint32_t cr1 = USART_CR1_FIFOEN | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_PEIE;
    if (frame == 9) {
        cr1 |= USART_CR1_M0;
    } else if (frame == 7) {
        cr1 |= USART_CR1_M1;
    }
    if (parity) {
        cr1 |= USART_CR1_PCE | (parity == 1 ? USART_CR1_PS : 0);
    }
    // Pins first, then the UART; what the change of the lines looked like to the receiver is
    // thrown away once the line has been idle for a character.
    uart_pins(1);
    UART7->CR1 = cr1;
    UART7->CR1 = cr1 | USART_CR1_UE;
    uint32_t settle = HAL_GetTick();
    while (HAL_GetTick() - settle < 2 + 12000 / baud) {
    }
    UART7->RQR = USART_RQR_RXFRQ;
    UART7->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_PECF | USART_ICR_NECF;
    uart_rx_in = uart_rx_out = 0;
    uart_overruns = uart_framing = uart_parity = 0;
    HAL_NVIC_ClearPendingIRQ(UART7_IRQn);
    HAL_NVIC_SetPriority(UART7_IRQn, DEFAULT_IRQ_PRIO, 0);
    HAL_NVIC_EnableIRQ(UART7_IRQn);
    uart_on = 1;
    uint32_t actual = clock / prescalers[index] / divider;
    uint8_t result[4] = {actual, actual >> 8, actual >> 16, actual >> 24};
    send(sequence, 0, result, 4);
}

static void handle_uart(uint8_t sequence, uint8_t operation, const uint8_t *args, size_t size) {
    static uint8_t data[MPY_M4_MAX_DATA];
    if (operation == OP_UART_INIT) {
        handle_uart_init(sequence, args, size);
        return;
    }
    if (operation == OP_UART_DEINIT) {
        uart_off();
        send(sequence, 0, NULL, 0);
        return;
    }
    if (!uart_on) {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    switch (operation) {
        case OP_UART_WRITE: {
            // A 9-bit character comes as two bytes, low byte first.
            size_t step = uart_wide ? 2 : 1, taken = 0;
            while (taken + step <= size) {
                uint32_t next = (uart_tx_in + 1) % UART_TX_SIZE;
                if (next == uart_tx_out) {
                    break;
                }
                uart_tx[uart_tx_in] = uart_wide ? get16(args + taken) & 0x1FF : args[taken];
                uart_tx_in = next;
                taken += step;
            }
            UART7->CR1 |= USART_CR1_TXEIE_TXFNFIE;
            uint8_t result[2] = {taken, taken >> 8};
            send(sequence, 0, result, 2);
            return;
        }
        case OP_UART_READ: {
            if (size != 2) {
                break;
            }
            size_t wanted = get16(args), length = 0;
            if (wanted > MPY_M4_MAX_DATA) {
                wanted = MPY_M4_MAX_DATA;
            }
            while (length < wanted && uart_rx_out != uart_rx_in) {
                uint16_t value = uart_rx[uart_rx_out];
                if (uart_wide) {
                    if (length + 2 > wanted) {
                        break;
                    }
                    data[length++] = value;
                    data[length++] = value >> 8;
                } else {
                    data[length++] = value;
                }
                uart_rx_out = (uart_rx_out + 1) % UART_RX_SIZE;
            }
            send(sequence, 0, data, length);
            return;
        }
        case OP_UART_STATUS: {
            uint32_t waiting = (uart_rx_in + UART_RX_SIZE - uart_rx_out) % UART_RX_SIZE;
            uint32_t pending = (uart_tx_in + UART_TX_SIZE - uart_tx_out) % UART_TX_SIZE;
            uint8_t done = pending == 0 && (UART7->ISR & USART_ISR_TC) ? 1 : 0;
            uint8_t result[11] = {waiting, waiting >> 8, pending, pending >> 8, done, uart_overruns,
                uart_overruns >> 8, uart_framing, uart_framing >> 8, uart_parity, uart_parity >> 8};
            send(sequence, 0, result, sizeof(result));
            return;
        }
        case OP_UART_BREAK:
            UART7->RQR = USART_RQR_SBKRQ;
            send(sequence, 0, NULL, 0);
            return;
    }
    send(sequence, E_INVAL, NULL, 0);
}

// ---- timers on the cycle counter, extended to 64 bits

static uint64_t cycles_now(void) {
    // DWT->CYCCNT wraps every 20 s at 209 MHz; the main loop calls this far more often.
    static uint32_t last;
    static uint64_t high;
    uint32_t now = DWT->CYCCNT;
    if (now < last) {
        high += 1ULL << 32;
    }
    last = now;
    return high | now;
}

static struct {
    uint8_t on, periodic, unacked;  // unacked: an event sent, Linux has not said it handled it
    uint32_t count;                 // expiries not reported yet
    uint64_t period, deadline;      // cycles
    uint64_t first;                 // when the oldest unreported expiry was due
} timers[MPY_M4_TIMERS];
static uint32_t core_hz;

static void event(uint8_t kind, const uint8_t *data, size_t size) {
    send(0, kind, data, size);
}

static void timers_poll(void) {
    uint64_t now = cycles_now();
    for (int i = 0; i < MPY_M4_TIMERS; ++i) {
        if (timers[i].on && now >= timers[i].deadline) {
            // Every expiry is counted, however late the loop got here.
            uint32_t count = 1;
            if (!timers[i].count) {
                timers[i].first = timers[i].deadline;
            }
            if (timers[i].periodic) {
                count += (now - timers[i].deadline) / timers[i].period;
                timers[i].deadline += (uint64_t)count * timers[i].period;
            } else {
                timers[i].on = 0;
            }
            timers[i].count += count;
        }
        if (timers[i].count && !timers[i].unacked) {
            uint32_t count = timers[i].count;
            uint32_t late_us = (now - timers[i].first) * 1000000 / core_hz;
            uint8_t data[9] = {i, count, count >> 8, count >> 16, count >> 24, late_us, late_us >> 8,
                late_us >> 16, late_us >> 24};
            event(EVENT_TIMER, data, sizeof(data));
            timers[i].count = 0;
            timers[i].unacked = 1;
        }
    }
}

static void handle_timer(uint8_t sequence, uint8_t operation, const uint8_t *args, size_t size) {
    if (operation == OP_EVENT_ACK) {
        for (int i = 0; i < MPY_M4_TIMERS; ++i) {
            timers[i].unacked = 0;
        }
        send(sequence, 0, NULL, 0);
        return;
    }
    if (operation == OP_TIMER_DEINIT && size == 1 && (args[0] < MPY_M4_TIMERS || args[0] == 0xFF)) {
        for (int i = 0; i < MPY_M4_TIMERS; ++i) {
            if (args[0] == 0xFF || args[0] == i) {
                timers[i].on = 0;
                timers[i].count = 0;
                timers[i].unacked = 0;
            }
        }
        send(sequence, 0, NULL, 0);
        return;
    }
    if (operation == OP_TIMER_INIT && size == 6 && args[0] < MPY_M4_TIMERS && args[1] <= 1 &&
        get32(args + 2) >= MPY_M4_TIMER_MIN_US) {
        int i = args[0];
        timers[i].on = 0;
        timers[i].count = 0;
        timers[i].unacked = 0;
        timers[i].periodic = args[1];
        timers[i].period = (uint64_t)get32(args + 2) * core_hz / 1000000;
        timers[i].deadline = cycles_now() + timers[i].period;
        timers[i].on = 1;
        send(sequence, 0, NULL, 0);
        return;
    }
    send(sequence, E_INVAL, NULL, 0);
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
    uint32_t start = DWT->CYCCNT, before = start, now, first_rise = 0, last_rise = 0;
    do {
        uint32_t level = gpio->IDR & mask;
        now = DWT->CYCCNT;
        samples++;
        if (last) {
            high += now - before;
        } else if (level) {
            if (!rising) {
                first_rise = now - start;
            }
            last_rise = now - start;
            rising++;
        }
        last = level;
        before = now;
    } while (now - start < length);
    uint32_t cycles = now - start;
    // The frequency is best taken between the first and the last rising edge (rising - 1 whole
    // periods): counting edges in the window is off by up to one.
    uint32_t values[7] = {samples, high, rising, cycles, core, first_rise, last_rise};
    send(sequence, 0, (const uint8_t *)values, sizeof(values));  // the M4 is little endian too
}

// ---- ADC sampling at a fixed rate: TIM2 -> ADC trigger, end of conversion -> buffer

static uint16_t capture[MPY_M4_CAPTURE];
static volatile uint32_t capture_count, capture_wanted, capture_overruns;
static volatile uint32_t capture_first, capture_last;  // DWT cycles of the first and last sample
static volatile uint8_t capture_state;                  // 0 idle, 1 running, 2 done
static ADC_TypeDef *capture_adc;

static void capture_end(void) {
    TIM2->CR1 = 0;
    LL_ADC_DisableIT_EOC(capture_adc);
    LL_ADC_DisableIT_OVR(capture_adc);
    if (LL_ADC_REG_IsConversionOngoing(capture_adc)) {
        LL_ADC_REG_StopConversion(capture_adc);
    }
}

static void capture_irq(void) {
    if (LL_ADC_IsActiveFlag_OVR(capture_adc)) {
        LL_ADC_ClearFlag_OVR(capture_adc);
        capture_overruns++;
    }
    if (LL_ADC_IsActiveFlag_EOC(capture_adc)) {
        uint32_t now = DWT->CYCCNT;
        uint16_t value = LL_ADC_REG_ReadConversionData32(capture_adc);  // clears EOC
        if (capture_state == 1 && capture_count < capture_wanted) {
            if (capture_count == 0) {
                capture_first = now;
            }
            capture_last = now;
            capture[capture_count++] = value;
            if (capture_count == capture_wanted) {
                capture_end();
                capture_state = 2;
            }
        }
    }
}

void ADC1_IRQHandler(void) {
    if (capture_adc == ADC1) {
        capture_irq();
    }
}

void ADC2_IRQHandler(void) {
    if (capture_adc == ADC2) {
        capture_irq();
    }
}

static void capture_finish(void) {
    // Back to software starts for ADC.read_u16() (needs no conversion going on).
    if (capture_adc && capture_state != 1) {
        HAL_NVIC_DisableIRQ(capture_adc == ADC1 ? ADC1_IRQn : ADC2_IRQn);
        while (LL_ADC_REG_IsStopConversionOngoing(capture_adc)) {
        }
        LL_ADC_REG_SetTriggerSource(capture_adc, LL_ADC_REG_TRIG_SOFTWARE);
        HAL_ADC_Stop(&adc[capture_adc == ADC1 ? 0 : 1]);
        capture_adc = NULL;
    }
}

static void handle_capture(uint8_t sequence, uint8_t operation, const uint8_t *args, size_t size) {
    if (operation == OP_ADC_CAPTURE_STATUS) {
        uint32_t cycles = capture_count > 1 ? capture_last - capture_first : 0;
        uint32_t values[3] = {capture_count, cycles, capture_overruns};
        uint8_t result[13];
        result[0] = capture_state;
        memcpy(result + 1, values, sizeof(values));
        send(sequence, 0, result, sizeof(result));
        return;
    }
    if (operation == OP_ADC_CAPTURE_FETCH) {
        uint32_t first = size == 6 ? get32(args) : 0;
        uint32_t count = size == 6 ? get16(args + 4) : 0;
        if (size != 6 || count > 200 || first + count > capture_count) {
            send(sequence, E_INVAL, NULL, 0);
            return;
        }
        send(sequence, 0, (const uint8_t *)(capture + first), 2 * count);  // little endian
        return;
    }
    // OP_ADC_CAPTURE
    int internal = args[0] == 2 && (args[1] == ADC_CHANNEL_CODE_VREFINT || args[1] == ADC_CHANNEL_CODE_TEMPSENSOR);
    if (size != 12 || (args[0] != 1 && args[0] != 2) || (args[1] >= 20 && !internal) || get32(args + 4) == 0 ||
        get32(args + 4) > MPY_M4_CAPTURE || get32(args + 8) == 0 || get32(args + 8) > MPY_M4_CAPTURE_MAX_HZ ||
        (args[2] != ADC_NO_PIN && (args[2] > 10 || args[3] > 15)) || capture_state == 1) {
        send(sequence, capture_state == 1 ? E_BUSY : E_INVAL, NULL, 0);
        return;
    }
    int number = args[0];
    uint32_t rate = get32(args + 8);
    if (args[2] != ADC_NO_PIN) {
        analog_pin(args[2], args[3]);
    }
    int error = adc_ready_for(number);
    if (error) {
        send(sequence, error, NULL, 0);
        return;
    }
    ADC_HandleTypeDef *h = &adc[number - 1];
    // The longest sampling time that leaves the conversion (8.5 more cycles at 16 bits) within
    // 70% of the period, at the 12 MHz ADC clock.
    static const uint32_t times[] = {ADC_SAMPLETIME_810CYCLES_5, ADC_SAMPLETIME_387CYCLES_5,
        ADC_SAMPLETIME_64CYCLES_5, ADC_SAMPLETIME_32CYCLES_5, ADC_SAMPLETIME_16CYCLES_5,
        ADC_SAMPLETIME_8CYCLES_5};
    static const uint32_t tenths[] = {8105, 3875, 645, 325, 165, 85};
    uint32_t budget = 12000000UL * 7 / 10 / rate * 10;  // ADC cycles x10 within 70% of a period
    int choice = 0;
    while (choice < 5 && tenths[choice] + 85 > budget) {
        choice++;
    }
    ADC_ChannelConfTypeDef config = {0};
    static const uint32_t inputs[20] = {ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
        ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7, ADC_CHANNEL_8, ADC_CHANNEL_9,
        ADC_CHANNEL_10, ADC_CHANNEL_11, ADC_CHANNEL_12, ADC_CHANNEL_13, ADC_CHANNEL_14, ADC_CHANNEL_15,
        ADC_CHANNEL_16, ADC_CHANNEL_17, ADC_CHANNEL_18, ADC_CHANNEL_19};
    config.Channel = internal ? (args[1] == ADC_CHANNEL_CODE_VREFINT ? ADC_CHANNEL_VREFINT : ADC_CHANNEL_TEMPSENSOR)
                              : inputs[args[1]];
    config.Rank = ADC_REGULAR_RANK_1;
    config.SamplingTime = times[choice];
    config.SingleDiff = ADC_SINGLE_ENDED;
    config.OffsetNumber = ADC_OFFSET_NONE;
    if (HAL_ADC_ConfigChannel(h, &config) != HAL_OK) {
        send(sequence, E_IO, NULL, 0);
        return;
    }
    // TIM2: 32 bits at its kernel clock, an update every period, the update as TRGO.
    uint32_t clock = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_TIMG1);
    if (!clock) {
        clock = 208877930;
    }
    uint32_t period = (clock + rate / 2) / rate;
    __HAL_RCC_TIM2_CLK_ENABLE();
    TIM2->CR1 = 0;
    TIM2->PSC = 0;
    TIM2->ARR = period - 1;
    TIM2->CR2 = 2U << TIM_CR2_MMS_Pos;  // TRGO on update
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0;

    capture_adc = h->Instance;
    capture_count = 0;
    capture_overruns = 0;
    capture_wanted = get32(args + 4);
    capture_state = 1;
    LL_ADC_REG_SetTriggerSource(capture_adc, LL_ADC_REG_TRIG_EXT_TIM2_TRGO);
    LL_ADC_REG_SetTriggerEdge(capture_adc, LL_ADC_REG_TRIG_EXT_RISING);
    LL_ADC_ClearFlag_EOC(capture_adc);
    LL_ADC_ClearFlag_OVR(capture_adc);
    LL_ADC_EnableIT_EOC(capture_adc);
    LL_ADC_EnableIT_OVR(capture_adc);
    IRQn_Type irq = number == 1 ? ADC1_IRQn : ADC2_IRQn;
    HAL_NVIC_ClearPendingIRQ(irq);
    HAL_NVIC_SetPriority(irq, 0, 0);  // above the IPCC: a sample is never late
    HAL_NVIC_EnableIRQ(irq);
    if (!LL_ADC_IsEnabled(capture_adc)) {
        LL_ADC_Enable(capture_adc);
        while (!LL_ADC_IsActiveFlag_ADRDY(capture_adc)) {
        }
    }
    LL_ADC_REG_StartConversion(capture_adc);  // waits for TIM2's triggers
    TIM2->CR1 = TIM_CR1_CEN;
    uint32_t result[3] = {clock, period, tenths[choice]};  // the rate is clock / period
    send(sequence, 0, (const uint8_t *)result, sizeof(result));
}

// ---- a GPIO port sampled at a fixed rate: a logic analyser

#define PORT_CAPTURE_MAX_HZ 4000000

static void handle_port_capture(uint8_t sequence, const uint8_t *args, size_t size) {
    if (size < 10 || args[9] > 2 || args[0] > 10 || get32(args + 1) == 0 || get32(args + 1) > PORT_CAPTURE_MAX_HZ ||
        get32(args + 5) == 0 || get32(args + 5) > MPY_M4_CAPTURE || capture_state == 1) {
        send(sequence, capture_state == 1 ? E_BUSY : E_INVAL, NULL, 0);
        return;
    }
    uint32_t rate = get32(args + 1), count = get32(args + 5);
    // At most a second: the M4 answers nothing else meanwhile.
    if ((uint64_t)count * 1000 / rate > 1000) {
        send(sequence, E_INVAL, NULL, 0);
        return;
    }
    const uint8_t *spi = args + 10;
    size_t spi_count = args[9] == 1 ? size - 10 : 0;
    if ((args[9] == 1 && spi_mode == 0xFF) || (args[9] == 2 && !uart_on)) {
        send(sequence, E_INVAL, NULL, 0);  // the bus is not set up: SPI(4) or UART(7) first
        return;
    }
    GPIO_TypeDef *gpio = (GPIO_TypeDef *)(GPIOA_BASE + args[0] * (GPIOB_BASE - GPIOA_BASE));
    SET_BIT(RCC->MC_AHB4ENSETR, 1U << args[0]);
    uint32_t period = core_hz / rate;
    size_t sent = 0;
    if (spi_count) {  // armed; the clock starts with the first byte written, after the lead
        SPI4->IFCR = 0xFF8;
        SPI4->CR2 = spi_count;
        SPI4->CR1 = SPI_CR1_SSI | SPI_CR1_SPE;
        SPI4->CR1 = SPI_CR1_SSI | SPI_CR1_SPE | SPI_CR1_CSTART;
    }
    // UART7: fed by this loop too, not by its interrupt, which would hold the sampling up.
    const uint8_t *uart = args + 10;
    size_t uart_count = args[9] == 2 ? size - 10 : 0, uart_sent = 0;
    // The buses start after the first 5% of the capture, so that their idle state shows first.
    uint32_t lead = count / 20;
    uint32_t late = 0, next = DWT->CYCCNT + period;
    for (uint32_t i = 0; i < count; ++i) {
        while ((int32_t)(DWT->CYCCNT - next) < 0) {
            if (i < lead) {
                continue;
            }
            if (uart_sent < uart_count && (UART7->ISR & USART_ISR_TXE_TXFNF)) {
                UART7->TDR = uart[uart_sent++];
            }
            if (spi_count) {
                uint32_t status = SPI4->SR;
                if (sent < spi_count && (status & SPI_SR_TXP)) {
                    *(volatile uint8_t *)&SPI4->TXDR = spi[sent++];
                }
                if (status & SPI_SR_RXP) {
                    (void)*(volatile uint8_t *)&SPI4->RXDR;
                }
            }
        }
        capture[i] = gpio->IDR;
        if ((int32_t)(DWT->CYCCNT - next) > (int32_t)period) {
            late++;  // an interrupt held the loop for more than a sample
        }
        next += period;
    }
    while (uart_sent < uart_count) {  // the rest, after the capture
        if (UART7->ISR & USART_ISR_TXE_TXFNF) {
            UART7->TDR = uart[uart_sent++];
        }
    }
    if (spi_count) {
        uint32_t wait = HAL_GetTick();
        while (sent < spi_count && HAL_GetTick() - wait < 100) {
            if (SPI4->SR & SPI_SR_TXP) {
                *(volatile uint8_t *)&SPI4->TXDR = spi[sent++];
            }
            if (SPI4->SR & SPI_SR_RXP) {
                (void)*(volatile uint8_t *)&SPI4->RXDR;
            }
        }
        while (!(SPI4->SR & SPI_SR_EOT) && HAL_GetTick() - wait < 100) {
            if (SPI4->SR & SPI_SR_RXP) {
                (void)*(volatile uint8_t *)&SPI4->RXDR;
            }
        }
        SPI4->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;
        SPI4->CR1 = SPI_CR1_SSI;
    }
    capture_count = count;
    capture_state = 2;  // for ADC_CAPTURE_FETCH
    uint32_t result[3] = {core_hz, period, late};
    send(sequence, 0, (const uint8_t *)result, sizeof(result));
}

// ---- requests

static void handle(uint8_t sequence, uint8_t operation, const uint8_t *args, size_t size) {
    switch (operation) {
        case OP_INFO: {
            static const uint8_t info[] = "\x02" FIRMWARE_TEXT;  // MPY_M4_PROTOCOL_VERSION, text
            _Static_assert(MPY_M4_PROTOCOL_VERSION == 2, "the version byte of info");
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
            capture_finish();
            if (capture_state == 1) {
                send(sequence, E_BUSY, NULL, 0);
                return;
            }
            handle_adc(sequence, args, size);
            return;
        case OP_ADC_CAPTURE:
            capture_finish();
            handle_capture(sequence, operation, args, size);
            return;
        case OP_ADC_CAPTURE_STATUS:
        case OP_ADC_CAPTURE_FETCH:
            handle_capture(sequence, operation, args, size);
            return;
        case OP_PWM_INIT:
        case OP_PWM_COMPARE:
        case OP_PWM_DEINIT:
        case OP_TONE_SEQUENCE:
        case OP_TONE_STATUS:
            handle_pwm(sequence, operation, args, size);
            return;
        case OP_SPI_INIT:
        case OP_SPI_TRANSFER:
        case OP_SPI_DEINIT:
            handle_spi(sequence, operation, args, size);
            return;
        case OP_UART_INIT:
        case OP_UART_WRITE:
        case OP_UART_READ:
        case OP_UART_STATUS:
        case OP_UART_BREAK:
        case OP_UART_DEINIT:
            handle_uart(sequence, operation, args, size);
            return;
        case OP_TIMER_INIT:
        case OP_TIMER_DEINIT:
        case OP_EVENT_ACK:
            handle_timer(sequence, operation, args, size);
            return;
        case OP_PORT_CAPTURE:
            capture_finish();
            handle_port_capture(sequence, args, size);
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

static void handle_received(inbox_t *inbox, int way) {
    while (inbox->count >= 2) {
        size_t length = get16(inbox->data);
        if (length < 2 || length > MPY_M4_MAX_FRAME) {
            inbox->count = 0;  // out of step: drop everything; Linux times out and goes on
            return;
        }
        if (inbox->count < 2 + length) {
            return;
        }
        channel_now = way;
        handle(inbox->data[2], inbox->data[3], inbox->data + 4, length - 2);
        inbox->count -= 2 + length;
        memmove(inbox->data, inbox->data + 2 + length, inbox->count);
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
    // Announced to Linux by name; with rpmsg_char it becomes /dev/rpmsg*.
    if (OPENAMP_create_endpoint(&raw, "rpmsg-raw", RPMSG_ADDR_ANY, on_raw, NULL) < 0) {
        Error_Handler();
    }

    core_hz = HAL_RCC_GetMCUFreq();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    while (1) {
        OPENAMP_check_for_message();
        handle_received(&received, CHANNEL_TTY);
        handle_received(&raw_received, CHANNEL_RAW);
        timers_poll();
        melody_poll();
    }
}
