// The app. Everything in this file can be replaced while the board keeps
// running: edit it, then run `make update`. The loader (main.cpp) stops this
// version, resets the hardware it used, and starts the new one.
//
// This one blinks the external LED on D7 and counts the blinks. The count is
// kept in the loader's state block, so it carries on across updates.
#include "api.hpp"

#define REG(addr) (*reinterpret_cast<volatile std::uint32_t*>(addr))
#define RCC_AHB1ENR REG(0x40023830)   // peripheral clock enable
#define GPIOA_MODER REG(0x40020000)   // pin modes for port A
#define GPIOA_ODR   REG(0x40020014)   // output values for port A

constexpr std::uint32_t LED = 1u << 8;         // PA8 = D7
constexpr std::uint32_t LED2 = 1u << 9;        // PA9 = D8

constexpr std::uint32_t PERIOD_MS = 200;       // time between LED on/off switches
const char* const NAME = "blinky v2";          // printed when it starts and stops

// Kept in the loader's state block, so it survives updates
struct State {
    std::uint32_t version;                     // change STATE_VERSION when you change this struct
    std::uint32_t blinks;
};
constexpr std::uint32_t STATE_VERSION = 1;
static_assert(sizeof(State) <= STATE_SIZE, "state block too small");

// Normal globals: they start fresh with every update
static const Api* api;
static State* state;
static std::uint32_t last_toggle, last_toggle2;

static void setup(const Api* a)
{
    api = a;
    state = static_cast<State*>(api->state);
    if (state->version != STATE_VERSION) {     // first run, or the layout changed
        state->version = STATE_VERSION;
        state->blinks = 0;
    }

    // The loader resets the hardware between apps, so set up everything we use
    RCC_AHB1ENR = RCC_AHB1ENR | (1u << 0);                    // GPIOA clock on
    GPIOA_MODER = (GPIOA_MODER & ~(3u << 16)) | (1u << 16);   // PA8 = output
    GPIOA_MODER = (GPIOA_MODER & ~(3u << 18)) | (1u << 18);   // PA9 = output
    last_toggle = api->millis();
    last_toggle2 = api->millis();

    api->print("[app] ");
    api->print(NAME);
    api->print(" started, blinks so far: ");
    api->print_num(state->blinks);
    api->print("\r\n");
}

static void loop()
{
    const std::uint32_t now = api->millis();
    if (now - last_toggle >= PERIOD_MS) {
        last_toggle = now;
        GPIOA_ODR = GPIOA_ODR ^ (LED);
        state->blinks++;
    }

    if (now - last_toggle2 >= PERIOD_MS * 2) {
        last_toggle2 = now;
        GPIOA_ODR = GPIOA_ODR ^ (LED2);
        state->blinks++;
    }
}

static void teardown()
{
    api->print("[app] ");
    api->print(NAME);
    api->print(" stopping\r\n");
}
//
extern "C" [[gnu::section(".update_header"), gnu::used]]
const AppHeader update_header = {APP_MAGIC, setup, loop, teardown};
