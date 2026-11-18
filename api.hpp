// The interface between the loader (main.cpp) and the app (update.cpp).
// Both are compiled against this file, so they always agree.
#pragma once
#include <cstdint>

// Services the loader offers the app
struct Api {
    void (*print)(const char* text);         // to the serial port
    void (*print_num)(std::uint32_t value);
    std::uint32_t (*millis)();               // milliseconds since the board started
    void* state;                             // STATE_SIZE bytes that survive swaps
};                                           // (zero after a reset)
constexpr std::uint32_t STATE_SIZE = 256;

// What every app starts with (update.ld puts it first in update.bin)
struct AppHeader {
    std::uint32_t magic;                     // APP_MAGIC
    void (*setup)(const Api* api);           // called once when the app starts
    void (*loop)();                          // called over and over, must return quickly
    void (*teardown)();                      // called before it's replaced, may be nullptr
};
constexpr std::uint32_t APP_MAGIC = 0x41505031;   // "APP1"

// Rules for apps:
// - loop() must return quickly: the loader checks for updates in between
// - don't touch USART2, pins PA2/PA3, SysTick or the clock setup: the loader uses them
// - no interrupts (an app that enables one is treated as crashed)
// - anything that should survive a swap goes in api->state, with a version
//   number so a new layout isn't misread
