# Nucleo-F401RE: swap the whole app while the board keeps running

A small permanent **loader** (`main.cpp`) and a replaceable **app**
(`update.cpp`). You flash the loader once; after that you change the app and
swap it in over USB serial, without resetting the board.

## Setup (once)

```sh
sudo apt install gcc-arm-none-eabi stlink-tools make picocom python3-serial
sudo usermod -aG dialout $USER     # then log out and back in
```

Wiring: D7 -> resistor (220-1k) -> LED long leg, LED short leg -> GND.

## Use

```sh
make flash      # once: put the loader on the board
make update     # build update.cpp and swap it in; repeat after every edit
make monitor    # optional: see what the board says (quit: Ctrl-A Ctrl-X)
```

Close picocom before `make update`, the upload needs the serial port.

The example app blinks D7 and counts the blinks. Try changing `PERIOD_MS` or
`NAME` in `update.cpp` and running `make update`: the old app prints that it's
stopping, the new one starts, and the blink count carries on.

## What happens on `make update`

1. The loader receives the new app into a separate buffer and checks it.
   A bad upload is rejected and the current app keeps running.
2. It calls the old app's `teardown()`.
3. It disables all interrupts and resets every peripheral the app could have
   used, except its own (USART2 on PA2/PA3, SysTick).
4. It copies the new app to RAM at `0x20010000` and calls its `setup()`.
5. It saves the app in flash, so it starts again after a reset or power-off.
   The LED pauses for about a quarter of a second while it saves.

If an app crashes (or turns on an interrupt), the board resets, deletes the
saved app and waits for a new one.

## Writing apps

The interface is in `api.hpp`. An app has `setup()`, `loop()` and an optional
`teardown()`, and gets `print`, `print_num`, `millis` and a 256-byte `state`
block from the loader. The rules:

- `loop()` must return quickly; the loader checks for updates in between.
- Set up all hardware you use in `setup()`; it's all reset between apps.
- Don't touch USART2, PA2/PA3, SysTick or the clock setup.
- No interrupts.
- Normal globals start fresh with every update. Anything that should carry
  over goes in `api->state`, with a version number: change it when you change
  the layout, and the app starts fresh instead of misreading old data.
  The state block is cleared on reset.

## Files

```
main.cpp         the loader
update.cpp       the app (blinks D7)
api.hpp          the interface between them
upload.py        PC side of `make update`
stm32f401re.ld   memory layout for the loader (flash sector 0, RAM, app area)
update.ld        memory layout for the app (built to run at 0x20010000)
build/           everything make produces; `make clean` deletes it
```

Flash: loader in sector 0 (max 16 KB, the linker checks), saved app in
sector 1 (`0x08004000`). App max size: 16 KB.
