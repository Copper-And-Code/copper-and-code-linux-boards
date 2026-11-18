# SPDX-License-Identifier: MIT
# Cortex-M4 firmware for the STM32MP157C-DK2, built with arm-none-eabi-gcc against STM32CubeMP1
# (HAL, OpenAMP, libmetal), without STM32CubeIDE. Linux loads the ELF through remoteproc.
#
#   make                       # ST's OpenAMP_TTY_echo, to check the RPMsg link
#   make APP=<folder> NAME=x   # an application folder with Src/ and Inc/ like ST's
#
# CUBE is a checkout of https://github.com/STMicroelectronics/STM32CubeMP1 (tag 1.7.0).

REPO := $(abspath ../..)
CUBE ?= $(REPO)/work/STM32CubeMP1
ECHO := $(CUBE)/Projects/STM32MP157C-DK2/Applications/OpenAMP/OpenAMP_TTY_echo
APP ?= $(ECHO)
NAME ?= openamp_tty_echo
BUILD ?= build/$(NAME)

CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
SIZE := $(CROSS)size

HAL := $(CUBE)/Drivers/STM32MP1xx_HAL_Driver
AMP := $(CUBE)/Middlewares/Third_Party/OpenAMP
IDE := $(ECHO)/STM32CubeIDE/CM4

SOURCES := $(wildcard $(APP)/Src/*.c) \
    $(IDE)/Application/User/syscalls.c \
    $(CUBE)/Drivers/BSP/STM32MP15xx_DISCO/stm32mp15xx_disco.c \
    $(addprefix $(HAL)/Src/stm32mp1xx_hal, .c _cortex.c _dma.c _dma_ex.c _gpio.c _hsem.c _i2c.c \
        _i2c_ex.c _ipcc.c _pwr.c _pwr_ex.c _rcc.c _rcc_ex.c $(EXTRA_HAL)) \
    $(addprefix $(AMP)/libmetal/lib/, device.c init.c io.c log.c shmem.c \
        system/generic/condition.c system/generic/generic_device.c system/generic/generic_init.c \
        system/generic/generic_io.c system/generic/generic_shmem.c system/generic/time.c \
        system/generic/template/sys.c) \
    $(addprefix $(AMP)/open-amp/lib/, remoteproc/remoteproc_virtio.c rpmsg/rpmsg.c \
        rpmsg/rpmsg_virtio.c virtio/virtio.c virtio/virtqueue.c) \
    $(AMP)/virtual_driver/virt_uart.c
STARTUP := $(IDE)/Application/Startup/startup_stm32mp157caax.s
LINKER := $(IDE)/STM32MP157CAAX_RAM.ld

INCLUDES := $(APP)/Inc $(HAL)/Inc $(HAL)/Inc/Legacy $(CUBE)/Drivers/CMSIS/Include \
    $(CUBE)/Drivers/CMSIS/Device/ST/STM32MP1xx/Include $(CUBE)/Drivers/BSP/STM32MP15xx_DISCO \
    $(AMP)/open-amp/lib/include $(AMP)/libmetal/lib/include $(AMP)/virtual_driver
DEFINES := CORE_CM4 USE_HAL_DRIVER STM32MP157Cxx METAL_INTERNAL METAL_MAX_DEVICE_REGIONS=2 \
    NO_ATOMIC_64_SUPPORT VIRTIO_SLAVE_ONLY __LOG_TRACE_IO_ $(EXTRA_DEFINES)

CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS := $(CPU) -Os -g -ffunction-sections -fdata-sections -Wall -std=gnu11 \
    $(addprefix -I,$(INCLUDES)) $(addprefix -D,$(DEFINES))
LDFLAGS := $(CPU) -T$(LINKER) --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections \
    -Wl,-Map=$(BUILD)/$(NAME).map -Wl,--no-warn-rwx-segments

OBJECTS := $(patsubst /%,$(BUILD)/obj/%.o,$(abspath $(SOURCES) $(STARTUP)))

$(BUILD)/$(NAME).elf: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lc -lm
	$(SIZE) $@

$(BUILD)/obj/%.c.o: /%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/obj/%.s.o: /%.s
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -c -o $@ $<

clean:
	rm -rf build

.PHONY: clean
