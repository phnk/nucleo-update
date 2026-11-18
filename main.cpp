// The loader. Flash it once with `make flash`. After that the app (update.cpp)
// can be replaced while the board keeps running, with `make update`.
//
// The loader only does what has to keep running during a swap:
//   - receive new apps over the serial port (USART2 = the ST-LINK's USB serial)
//   - stop the old app and reset the hardware it used, then start the new one
//   - save the app in flash and start it again after a reset
//   - recover if an app crashes
// Everything else is the app's job. The interface is in api.hpp.
// Runs on the default 16 MHz internal clock.
#include <array>
#include <cstdint>
#include "api.hpp"

// ---- Registers (from reference manual RM0368) ---------------------------
#define REG(addr) (*reinterpret_cast<volatile std::uint32_t*>(addr))
#define RCC_AHB1RSTR REG(0x40023810)  // peripheral reset (fast bus)
#define RCC_AHB2RSTR REG(0x40023814)
#define RCC_APB1RSTR REG(0x40023820)  // peripheral reset (slow buses)
#define RCC_APB2RSTR REG(0x40023824)
#define RCC_AHB1ENR  REG(0x40023830)  // peripheral clock enable
#define RCC_AHB2ENR  REG(0x40023834)
#define RCC_APB1ENR  REG(0x40023840)
#define RCC_APB2ENR  REG(0x40023844)
#define GPIOA_MODER   REG(0x40020000) // port A: pin modes
#define GPIOA_OTYPER  REG(0x40020004) //         output type
#define GPIOA_OSPEEDR REG(0x40020008) //         speed
#define GPIOA_PUPDR   REG(0x4002000C) //         pull-up/down
#define GPIOA_ODR     REG(0x40020014) //         output values
#define GPIOA_AFRL    REG(0x40020020) //         alternate functions, pins 0-7
#define GPIOA_AFRH    REG(0x40020024) //         alternate functions, pins 8-15
#define USART2_SR    REG(0x40004400)  // UART status
#define USART2_DR    REG(0x40004404)  // UART data
#define USART2_BRR   REG(0x40004408)  // UART baud rate
#define USART2_CR1   REG(0x4000440C)  // UART control
#define SYST_CSR     REG(0xE000E010)  // SysTick timer control/status
#define SYST_RVR     REG(0xE000E014)  // SysTick reload value
#define SYST_CVR     REG(0xE000E018)  // SysTick current value
#define NVIC_ICER(n) REG(0xE000E180 + 4 * (n))  // interrupt disable
#define NVIC_ICPR(n) REG(0xE000E280 + 4 * (n))  // interrupt clear-pending
#define SCB_AIRCR    REG(0xE000ED0C)  // used to reset the chip

// ---- Time ------------------------------------------------------------------
volatile std::uint32_t ms_ticks;

extern "C" void SysTick_Handler()             // runs every millisecond
{
    ms_ticks = ms_ticks + 1;
}

std::uint32_t millis()
{
    return ms_ticks;
}

// ---- UART: USART2 on PA2/PA3, 115200 baud ------------------------------------
void uart_putc(char c)
{
    while (!(USART2_SR & (1u << 7))) {}           // wait until ready for next byte
    USART2_DR = static_cast<std::uint8_t>(c);
}

void uart_print(const char* s)
{
    while (*s)
        uart_putc(*s++);
}

void uart_print_num(std::uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    do {
        buf[--i] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    uart_print(buf + i);
}

// Returns the received byte, or -1 if nothing has arrived
int uart_getc()
{
    if (USART2_SR & (1u << 5))
        return static_cast<int>(USART2_DR & 0xFF);
    return -1;
}

int uart_getc_timeout(std::uint32_t ms)
{
    const std::uint32_t start = millis();
    while (millis() - start < ms) {
        int c = uart_getc();
        if (c >= 0)
            return c;
    }
    return -1;
}

// ---- Board setup: only what the loader itself uses -----------------------------
void board_init()
{
    RCC_AHB1ENR = RCC_AHB1ENR | (1u << 0);                      // GPIOA clock on
    RCC_APB1ENR = RCC_APB1ENR | (1u << 17);                     // USART2 clock on
    GPIOA_MODER = (GPIOA_MODER & ~(0xFu << 4)) | (0xAu << 4);   // PA2, PA3 = alternate function
    GPIOA_AFRL  = (GPIOA_AFRL & ~(0xFFu << 8)) | (0x77u << 8);  // ...AF7 = USART2
    USART2_BRR  = 139;                                          // 16 MHz / 115200
    USART2_CR1  = (1u << 13) | (1u << 3) | (1u << 2);           // enable, TX, RX

    SYST_RVR = 16000 - 1;                                       // 1 ms at 16 MHz
    SYST_CVR = 0;
    SYST_CSR = 0x7;                                             // CPU clock, interrupt, enable
}

// Reset the chip, like pressing the RESET button
[[noreturn]] void system_reset()
{
    while (!(USART2_SR & (1u << 6))) {}           // let the last character go out
    asm volatile("dsb" ::: "memory");
    SCB_AIRCR = 0x05FA0004;                        // key + SYSRESETREQ
    while (true) {}
}

// Put everything an app may have used back to its power-on state, except
// what the loader itself uses (USART2 on PA2/PA3, SysTick, flash)
void reset_app_hardware()
{
    // Interrupts: disable and clear all 85 of them (3 registers x 32 bits)
    for (int n = 0; n < 3; n++) {
        NVIC_ICER(n) = 0xFFFFFFFF;
        NVIC_ICPR(n) = 0xFFFFFFFF;
    }
    asm volatile("cpsie i");                       // in case the app turned interrupts off

    // Peripherals: hold them in reset, then release. Bit numbers from RM0368
    // section 6.3. Not GPIOA and not USART2: the loader's UART uses them.
    constexpr std::uint32_t ahb1 = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4)   // GPIOB-E
                                 | (1u << 7)                                       // GPIOH
                                 | (1u << 12)                                      // CRC
                                 | (1u << 21) | (1u << 22);                        // DMA1, DMA2
    constexpr std::uint32_t ahb2 = (1u << 7);                                      // USB
    constexpr std::uint32_t apb1 = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3)   // TIM2-5
                                 | (1u << 11)                                      // WWDG
                                 | (1u << 14) | (1u << 15)                         // SPI2, SPI3
                                 | (1u << 21) | (1u << 22) | (1u << 23)            // I2C1-3
                                 | (1u << 28);                                     // PWR
    constexpr std::uint32_t apb2 = (1u << 0)                                       // TIM1
                                 | (1u << 4) | (1u << 5)                           // USART1, USART6
                                 | (1u << 8)                                       // ADC1
                                 | (1u << 11)                                      // SDIO
                                 | (1u << 12) | (1u << 13)                         // SPI1, SPI4
                                 | (1u << 14)                                      // SYSCFG
                                 | (1u << 16) | (1u << 17) | (1u << 18);           // TIM9-11
    static_assert((apb1 & (1u << 17)) == 0, "must not reset USART2");
    static_assert((ahb1 & (1u << 0)) == 0, "must not reset GPIOA");
    RCC_AHB1RSTR = ahb1;
    RCC_AHB2RSTR = ahb2;
    RCC_APB1RSTR = apb1;
    RCC_APB2RSTR = apb2;
    RCC_AHB1RSTR = 0;
    RCC_AHB2RSTR = 0;
    RCC_APB1RSTR = 0;
    RCC_APB2RSTR = 0;

    // Clocks: only the loader's
    RCC_AHB1ENR = 1u << 0;                         // GPIOA
    RCC_AHB2ENR = 0;
    RCC_APB1ENR = 1u << 17;                        // USART2
    RCC_APB2ENR = 0;

    // Port A can't be reset (the UART pins are on it), so put its other
    // pins back to their power-on values by hand
    GPIOA_MODER   = 0xA8000000 | (0xAu << 4);      // reset value + PA2/PA3 alternate function
    GPIOA_OTYPER  = 0;
    GPIOA_OSPEEDR = 0x0C000000;
    GPIOA_PUPDR   = 0x64000000;
    GPIOA_ODR     = 0;
    GPIOA_AFRL    = 0x77u << 8;                    // PA2/PA3 = AF7
    GPIOA_AFRH    = 0;
}

// ---- The app ---------------------------------------------------------------------
constexpr std::uint32_t SLOT_SIZE = 16 * 1024;
constexpr std::uint32_t MAX_APP = SLOT_SIZE - 8;  // must fit in flash with length + crc

[[gnu::section(".update_slot")]] std::uint8_t app_slot[SLOT_SIZE];  // the app runs here
std::uint8_t staging[SLOT_SIZE];                                    // uploads land here first
std::uint32_t app_state[STATE_SIZE / 4];                            // survives swaps

const Api api = {uart_print, uart_print_num, millis, app_state};
const AppHeader* app = nullptr;                    // the running app, if any

bool in_slot(const void* p)
{
    const auto a = reinterpret_cast<std::uintptr_t>(p) & ~1u;   // ignore the Thumb bit
    const auto s = reinterpret_cast<std::uintptr_t>(app_slot);
    return a >= s && a < s + SLOT_SIZE;
}

bool is_valid_app(const std::uint8_t* data)
{
    const auto* h = reinterpret_cast<const AppHeader*>(data);
    return h->magic == APP_MAGIC
        && in_slot(reinterpret_cast<const void*>(h->setup))
        && in_slot(reinterpret_cast<const void*>(h->loop))
        && (h->teardown == nullptr || in_slot(reinterpret_cast<const void*>(h->teardown)));
}

void stop_app()
{
    if (app && app->teardown)
        app->teardown();
    app = nullptr;
    reset_app_hardware();
}

void start_app(const std::uint8_t* data, std::uint32_t size)
{
    for (std::uint32_t i = 0; i < size; i++)       // copy to where it was built to run
        app_slot[i] = data[i];
    asm volatile("dsb\n isb" ::: "memory");        // make sure the CPU sees the new code
    app = reinterpret_cast<const AppHeader*>(app_slot);
    app->setup(&api);
}

std::uint32_t crc32(const std::uint8_t* data, std::uint32_t n)
{
    std::uint32_t crc = 0xFFFFFFFF;
    for (std::uint32_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// ---- Saving the app in flash -------------------------------------------------------
// Flash sector 1 (16 KB at 0x08004000) holds the app:
//   word 0: length, word 1: crc32, then the app itself.
// The loader lives in sector 0 (see stm32f401re.ld).
constexpr std::uint32_t SAVED_ADDR   = 0x08004000;
constexpr std::uint32_t SAVED_SECTOR = 1;

#define FLASH_KEYR REG(0x40023C04)
#define FLASH_SR   REG(0x40023C0C)
#define FLASH_CR   REG(0x40023C10)

void flash_unlock()
{
    if (FLASH_CR & (1u << 31)) {                   // LOCK bit
        FLASH_KEYR = 0x45670123;                   // magic key sequence
        FLASH_KEYR = 0xCDEF89AB;
    }
}

void flash_lock()
{
    FLASH_CR = 1u << 31;
}

// Wait for the flash to finish, return false if it reported an error
bool flash_wait()
{
    while (FLASH_SR & (1u << 16)) {}               // BSY
    const bool ok = !(FLASH_SR & 0x1F2);           // error flags
    FLASH_SR = 0x1F3;                              // clear the flags
    return ok;
}

// Erase sector 1. The CPU pauses while this happens (~0.25 s).
bool flash_erase_saved()
{
    flash_wait();
    FLASH_CR = (1u << 1) | (SAVED_SECTOR << 3) | (2u << 8);   // sector erase, 32-bit
    FLASH_CR = FLASH_CR | (1u << 16);                         // start
    const bool ok = flash_wait();
    FLASH_CR = 0;
    return ok;
}

bool flash_write_word(std::uint32_t addr, std::uint32_t word)
{
    FLASH_CR = (1u << 0) | (2u << 8);              // program, 32-bit
    REG(addr) = word;
    const bool ok = flash_wait();
    FLASH_CR = 0;
    return ok && REG(addr) == word;                // read back to be sure
}

bool save_app(const std::uint8_t* data, std::uint32_t size)
{
    flash_unlock();
    bool ok = flash_erase_saved()
           && flash_write_word(SAVED_ADDR, size)
           && flash_write_word(SAVED_ADDR + 4, crc32(data, size));
    for (std::uint32_t i = 0; ok && i < size; i += 4) {
        std::uint32_t word = 0xFFFFFFFF;           // pad the last word
        for (std::uint32_t k = 0; k < 4 && i + k < size; k++)
            word = (word & ~(0xFFu << (8 * k))) | (static_cast<std::uint32_t>(data[i + k]) << (8 * k));
        ok = flash_write_word(SAVED_ADDR + 8 + i, word);
    }
    flash_lock();
    return ok;
}

// Size of the app saved in flash, or 0 if there isn't a valid one
std::uint32_t saved_app_size()
{
    const std::uint32_t size = REG(SAVED_ADDR);    // 0xFFFFFFFF if nothing saved
    const auto* data = reinterpret_cast<const std::uint8_t*>(SAVED_ADDR + 8);
    if (size < sizeof(AppHeader) || size > MAX_APP
        || crc32(data, size) != REG(SAVED_ADDR + 4) || !is_valid_app(data))
        return 0;
    return size;
}

void forget_saved_app()
{
    flash_unlock();
    flash_erase_saved();
    flash_lock();
}

// Set by the crash handler, survives the reset after a crash
[[gnu::section(".noinit")]] volatile std::uint32_t crash_flag;
constexpr std::uint32_t CRASHED = 0xC4A5C4A5;

// ---- Upload protocol (the PC side is upload.py) -------------------------------------
//   PC sends 'U' until the board answers ACK
//   PC sends length and CRC32 (4 bytes each, little endian)    -> ACK / NACK
//   PC sends the data in 256-byte chunks, waits for ACK after each one
//   board checks everything and answers ACK / NACK at the end
constexpr char SYNC = 'U';
constexpr char ACK  = 0x06;
constexpr char NACK = 0x15;
constexpr std::uint32_t CHUNK = 256;

// Receive an upload into `staging`, so a failed upload can't break the app
// that's currently running. Returns the length, or 0 if something went wrong.
std::uint32_t receive_upload()
{
    uart_putc(ACK);
    while (uart_getc_timeout(20) >= 0) {}          // skip extra 'U's sent before our ACK

    std::uint32_t header[2] = {0, 0};              // length, crc
    for (int i = 0; i < 8; i++) {
        int c = uart_getc_timeout(1000);
        if (c < 0)
            return 0;
        header[i / 4] |= static_cast<std::uint32_t>(c) << (8 * (i % 4));
    }
    const std::uint32_t length = header[0];
    if (length < sizeof(AppHeader) || length > MAX_APP) {
        uart_putc(NACK);
        return 0;
    }
    uart_putc(ACK);

    for (std::uint32_t offset = 0; offset < length; offset += CHUNK) {
        const std::uint32_t n = (length - offset < CHUNK) ? length - offset : CHUNK;
        for (std::uint32_t i = 0; i < n; i++) {
            int c = uart_getc_timeout(1000);
            if (c < 0)
                return 0;
            staging[offset + i] = static_cast<std::uint8_t>(c);
        }
        uart_putc(ACK);
    }
    return (crc32(staging, length) == header[1]) ? length : 0;
}

void handle_upload()
{
    asm volatile("cpsie i");                       // millis() needs interrupts on

    const std::uint32_t size = receive_upload();
    if (size == 0 || !is_valid_app(staging)) {
        uart_putc(NACK);
        uart_print("[loader] upload failed, keeping the current app\r\n");
        return;
    }
    uart_putc(ACK);

    uart_print("[loader] swapping in the new app\r\n");
    stop_app();                                    // teardown + reset its hardware
    start_app(staging, size);                      // copy + setup()
    uart_print(save_app(staging, size)
                   ? "[loader] saved to flash, survives reset and power-off\r\n"
                   : "[loader] couldn't save to flash, lost on reset\r\n");
}

// ---- Main ---------------------------------------------------------------------------
int main()
{
    board_init();
    uart_print("\r\n[loader] started\r\n");

    if (crash_flag == CRASHED) {
        forget_saved_app();
        uart_print("[loader] the last app crashed, deleted it\r\n");
    }
    crash_flag = 0;

    if (const std::uint32_t size = saved_app_size()) {
        uart_print("[loader] starting the saved app\r\n");
        start_app(reinterpret_cast<const std::uint8_t*>(SAVED_ADDR + 8), size);
    } else {
        uart_print("[loader] no app yet, send one with: make update\r\n");
    }

    while (true) {
        if (app)
            app->loop();
        if (uart_getc() == SYNC)                   // upload.py wants to send an app
            handle_upload();
    }
}

// ---- Startup: what runs before main() ------------------------------------------------
extern "C" std::uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;

extern "C" void Reset_Handler()
{
    std::uint32_t* src = &_sidata;                 // copy initial values of globals
    for (std::uint32_t* dst = &_sdata; dst < &_edata;) *dst++ = *src++;
    for (std::uint32_t* dst = &_sbss; dst < &_ebss;) *dst++ = 0;  // zero the rest

    main();
    while (true) {}
}

// Runs if anything crashes, or an app uses an interrupt it isn't allowed to.
// Resets; at startup main() sees crash_flag and deletes the saved app.
extern "C" void HardFault_Handler()
{
    crash_flag = CRASHED;
    if (RCC_APB1ENR & (1u << 17))
        uart_print("\r\n*** crashed, resetting ***\r\n");
    system_reset();
}

// Vector table, placed at the start of flash: initial stack pointer, then the
// handlers for the 15 core exceptions and 85 interrupts. Everything except
// reset and SysTick goes to the crash handler.
using Handler = void (*)();

constexpr std::array<Handler, 15 + 85> make_handlers()
{
    std::array<Handler, 15 + 85> h{};
    for (auto& entry : h)
        entry = HardFault_Handler;
    h[0] = Reset_Handler;
    h[6] = h[7] = h[8] = h[9] = h[12] = nullptr;   // reserved slots
    h[14] = SysTick_Handler;
    return h;
}

struct VectorTable {
    const void* initial_sp;
    std::array<Handler, 15 + 85> handlers;
};

[[gnu::section(".isr_vector"), gnu::used]]
constinit const VectorTable vector_table{&_estack, make_handlers()};
