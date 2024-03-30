# Compiler
CC = avr-gcc
OBJCOPY = avr-objcopy

# Compiler flags
CFLAGS = -mmcu=atmega8 -DF_CPU=100000UL -Wall -g -Os -std=gnu11 --param=min-pagesize=0 -I${AVR_PATH}/include

# Source files
SRC = boiler.c

# Object files
OBJ = $(SRC:.c=.o)

# Executable name
TARGET = boiler.elf
HEX = boiler.hex

.PHONY: all clean

all: $(TARGET) $(HEX)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(HEX): $(TARGET)
	$(OBJCOPY) -O ihex $< $@

clean:
	rm -f $(OBJ) $(TARGET) $(HEX)
