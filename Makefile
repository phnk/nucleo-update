CXX      = arm-none-eabi-g++
OBJCOPY  = arm-none-eabi-objcopy
CXXFLAGS = -std=c++20 -mcpu=cortex-m4 -mthumb -O2 -g -Wall -fno-exceptions -fno-rtti -nostdlib \
           -fno-tree-loop-distribute-patterns  # don't turn loops into memcpy (no libc)
PORT     = /dev/ttyACM0
BUILD    = build

all: $(BUILD)/main.bin $(BUILD)/update.bin

# .cpp -> .elf (has debug info) -> raw .bin, all inside build/
$(BUILD)/main.elf: main.cpp api.hpp stm32f401re.ld | $(BUILD)
	$(CXX) $(CXXFLAGS) -T stm32f401re.ld main.cpp -o $@

$(BUILD)/update.elf: update.cpp api.hpp update.ld | $(BUILD)
	$(CXX) $(CXXFLAGS) -T update.ld update.cpp -o $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD):
	mkdir -p $@

# Once: put the loader in flash
flash: $(BUILD)/main.bin
	st-flash --reset write $< 0x08000000

# Whenever you change update.cpp: swap it into the running board, no reset
update: $(BUILD)/update.bin
	python3 upload.py $< $(PORT)

monitor:
	picocom -b 115200 $(PORT)

clean:
	rm -rf $(BUILD)

.PRECIOUS: $(BUILD)/%.elf
.PHONY: all flash update monitor clean
