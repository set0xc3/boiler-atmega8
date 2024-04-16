# Compiler and flags
CC = avr-gcc
OBJCOPY = avr-objcopy
SIZE = avr-size
CFLAGS = -mmcu=atmega8 -DF_CPU=1000000UL -Wall -Os -std=gnu11 --param=min-pagesize=0 -I${AVR_PATH}/include

FIRMWARE_NAME = boiler

.PHONY: build clean

all: clean build

build: $(FIRMWARE_NAME).bin

$(FIRMWARE_NAME).bin: $(FIRMWARE_NAME).elf
	$(OBJCOPY) $< -O ihex $@

$(FIRMWARE_NAME).elf: $(FIRMWARE_NAME).c
	$(CC) $< -o $@ $(CFLAGS)

size: $(FIRMWARE_NAME).elf
	$(SIZE) $<

flash: $(FIRMWARE_NAME).bin
	 avrdude -c usbasp -p m8 -U flash:w:$<:a

clean:
	rm -f *.elf *.bin
