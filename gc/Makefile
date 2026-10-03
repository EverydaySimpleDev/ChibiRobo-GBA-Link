# Chibi-Robo! GBA Link - GameCube code injected into main.dol (devkitPPC).
# Output: build/chibi_link.bin (raw section) + build/chibi_link.sym (nm listing)
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
NM      := $(PREFIX)nm

GBA_ROM := ../gba/chibi_link_mb.gba
BUILD   := build

CFLAGS  := -O2 -Wall -mcpu=750 -meabi -mhard-float -G0 -msdata=none \
           -ffreestanding -fno-builtin -fno-asynchronous-unwind-tables \
           -fno-exceptions -I../shared -Isource
ASFLAGS := -mcpu=750 -meabi -DGBA_ROM_PATH='"$(abspath $(GBA_ROM))"'
LDFLAGS := -nostdlib -Wl,-T,link.ld -Wl,-Map,$(BUILD)/chibi_link.map -Wl,--gc-sections

OBJS := $(BUILD)/gbalink.o $(BUILD)/messages.o $(BUILD)/gba_rom.o

.PHONY: all clean
all: $(BUILD)/chibi_link.bin $(BUILD)/chibi_link.sym

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: source/%.c source/game.h source/messages.h ../shared/chibi_link_protocol.h ../shared/chibi_link_pins.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/gba_rom.o: source/gba_rom.S $(GBA_ROM) | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/chibi_link.elf: $(OBJS) link.ld game_symbols.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -lgcc -o $@

$(BUILD)/chibi_link.bin: $(BUILD)/chibi_link.elf
	$(OBJCOPY) -O binary $< $@
	@echo built ... $@ \($$(stat -c %s $@) bytes\)

$(BUILD)/chibi_link.sym: $(BUILD)/chibi_link.elf
	$(NM) $< > $@

$(GBA_ROM):
	$(MAKE) -C ../gba

clean:
	rm -rf $(BUILD)
