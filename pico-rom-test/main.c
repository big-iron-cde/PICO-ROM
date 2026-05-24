/*
 * pico-rom-test — Pico-as-ROM firmware for the breadboard 6502 prototype
 *
 * Connects to a W65C02S + HM62256LP system on a breadboard. Provides a
 * USB-CDC command interface for hardware bring-up plus a real 32 KB ROM
 * image that the CPU sees mapped at $8000-$FFFF.
 *
 *   PHI2 clock generation     →  c1 / c2 / c4 / c<khz> / cs
 *   RESET drive               →  r0 / r1
 *   Address bus sampling      →  a / am / as
 *   ROM emulation             →  rom / roms     (serves rom_image[])
 *   ROM upload (binary)       →  loadbin        (host pipes 32 KB)
 *   Bus watch (print port)    →  watch <hex> / unwatch
 *
 * Connect over USB serial at any baud (USB-CDC ignores it) and type the
 * commands followed by Enter. See upload-rom.py for pushing rom.bin.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"

// ─── Pin map (matches pico-as-rom-wiring.md) ─────────────────────────────

#define PIN_A_FIRST   0    // GP0..GP14 = A0..A14 (15 pins)
#define PIN_A_LAST    14
#define PIN_D_FIRST   15   // GP15..GP22 = D0..D7 (8 pins)
#define PIN_D_LAST    22
#define PIN_A15       26   // GP26 = A15 (used as Pico's chip-enable)
#define PIN_RESET     27   // GP27 = RESET drive (open-drain emulated)
#define PIN_PHI2      28   // GP28 = PHI2 clock output

#define DATA_MASK     (0xFFu << PIN_D_FIRST)
#define ADDR_LOW_MASK (0x7FFFu << PIN_A_FIRST)

#define ROM_SIZE      0x8000   // 32 KB — fits CPU $8000-$FFFF

// ─── State ───────────────────────────────────────────────────────────────

static uint8_t rom_image[ROM_SIZE];

static bool monitor_addr = false;
static bool rom_active   = false;

// Watch: when the CPU bus address matches `watch_addr`, the data byte on the
// bus at that moment is forwarded over USB. Useful for treating a specific
// memory address as a virtual "print port".
static bool     watch_active = false;
static uint16_t watch_addr   = 0x4000;
static volatile uint8_t watch_pending_data = 0;
static volatile bool    watch_pending      = false;
static uint8_t  watch_last_printed = 0xFF;
static bool     watch_have_printed = false;

// ─── Pin setup ───────────────────────────────────────────────────────────

static void pins_init(void) {
    // Address bus + A15 + reset start as inputs (safe — won't fight bus)
    for (int p = PIN_A_FIRST; p <= PIN_A_LAST; p++) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
    }
    for (int p = PIN_D_FIRST; p <= PIN_D_LAST; p++) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
    }
    gpio_init(PIN_A15);
    gpio_set_dir(PIN_A15, GPIO_IN);

    // RESET starts asserted (output LOW = CPU held in reset)
    gpio_init(PIN_RESET);
    gpio_set_dir(PIN_RESET, GPIO_OUT);
    gpio_put(PIN_RESET, 0);

    // PHI2 starts as PWM but stopped
    gpio_init(PIN_PHI2);
    gpio_set_dir(PIN_PHI2, GPIO_OUT);
    gpio_put(PIN_PHI2, 0);
}

// ─── PHI2 (clock) ────────────────────────────────────────────────────────

static void phi2_start(uint32_t target_hz) {
    gpio_set_function(PIN_PHI2, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_PHI2);
    uint chan  = pwm_gpio_to_channel(PIN_PHI2);

    // Pico 2 default sys clock = 150 MHz. Pico (RP2040) = 125 MHz.
    // We'll pick a wrap value that gives us target_hz square wave.
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t wrap   = sys_hz / target_hz;
    if (wrap < 4)    wrap = 4;
    if (wrap > 65535) wrap = 65535;

    pwm_set_clkdiv(slice, 1.0f);
    pwm_set_wrap(slice, wrap - 1);
    pwm_set_chan_level(slice, chan, wrap / 2);  // 50% duty
    pwm_set_enabled(slice, true);

    printf("PHI2 on: target %lu Hz, actual %lu Hz (wrap=%lu)\n",
           (unsigned long)target_hz,
           (unsigned long)(sys_hz / wrap),
           (unsigned long)wrap);
}

static void phi2_stop(void) {
    uint slice = pwm_gpio_to_slice_num(PIN_PHI2);
    pwm_set_enabled(slice, false);
    gpio_set_function(PIN_PHI2, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_PHI2, GPIO_OUT);
    gpio_put(PIN_PHI2, 0);
    printf("PHI2 off (held low)\n");
}

// ─── Reset ───────────────────────────────────────────────────────────────

static void reset_assert(void) {
    gpio_set_dir(PIN_RESET, GPIO_OUT);
    gpio_put(PIN_RESET, 0);
    printf("RESET asserted (CPU held in reset)\n");
}

static void reset_release(void) {
    // Open-drain emulation: switch to INPUT so the pull-up brings line high
    gpio_set_dir(PIN_RESET, GPIO_IN);
    printf("RESET released (CPU should run)\n");
}

// ─── Address bus sampling ───────────────────────────────────────────────

static uint16_t addr_read(void) {
    uint32_t pins = gpio_get_all();
    uint16_t addr = (pins >> PIN_A_FIRST) & 0x7FFFu;
    if (pins & (1u << PIN_A15)) addr |= 0x8000u;
    return addr;
}

static uint8_t data_read(void) {
    uint32_t pins = gpio_get_all();
    return (uint8_t)((pins >> PIN_D_FIRST) & 0xFFu);
}

// ─── ROM image ──────────────────────────────────────────────────────────
// rom_image[] holds the 32 KB ROM contents that the CPU sees mapped at
// $8000-$FFFF. On boot it's filled with $EA (NOPs) so the CPU walks
// straight through ROM forever — same behaviour as before `loadbin`.

static void rom_image_init(void) {
    memset(rom_image, 0xEA, sizeof(rom_image));
    // Default reset vector: $FFFC/$FFFD → $8000
    rom_image[0xFFFC - 0x8000] = 0x00;
    rom_image[0xFFFD - 0x8000] = 0x80;
    // Default IRQ/BRK vector: $FFFE/$FFFF → $8000
    rom_image[0xFFFE - 0x8000] = 0x00;
    rom_image[0xFFFF - 0x8000] = 0x80;
}

// ─── ROM emulation (polling) ────────────────────────────────────────────
// Each iteration: snapshot the bus, drive data when CPU is reading ROM,
// and capture watch events. Critical that this stays fast — no printf
// in here. Watch prints happen later from the main loop.

static void rom_task(void) {
    uint32_t pins = gpio_get_all();
    bool     a15  = (pins >> PIN_A15) & 1u;

    if (rom_active) {
        if (a15) {
            uint16_t addr = (pins >> PIN_A_FIRST) & 0x7FFFu;
            uint8_t  byte = rom_image[addr];
            gpio_set_dir_out_masked(DATA_MASK);
            gpio_put_masked(DATA_MASK, (uint32_t)byte << PIN_D_FIRST);
        } else {
            gpio_set_dir_in_masked(DATA_MASK);
        }
    }

    if (watch_active) {
        uint16_t addr = (pins >> PIN_A_FIRST) & 0x7FFFu;
        if (a15) addr |= 0x8000u;
        if (addr == watch_addr) {
            uint8_t data = (uint8_t)((pins >> PIN_D_FIRST) & 0xFFu);
            // Breadboard crosstalk: the high address byte often bleeds onto D0-D7
            // during writes (e.g. watch $4000 → spurious $40, watch $5000 → $50).
            if (data == (uint8_t)(watch_addr >> 8)) {
                return;
            }
            watch_pending_data = data;
            watch_pending      = true;
        }
    }
}

// ─── ROM upload (raw binary over USB-CDC) ───────────────────────────────
// Protocol:
//   host → "loadbin\n"
//   pico → "OK send 32768 bytes\n"
//   host → <32768 raw bytes>
//   pico → "loaded 32768 bytes\n"
//
// ROM emulation is paused for the duration of the upload so we don't fight
// the CPU's data bus. If the host stalls for >2 s the upload aborts.

static void cmd_loadbin(void) {
    printf("OK send %d bytes\n", ROM_SIZE);
    stdio_flush();

    bool was_active = rom_active;
    rom_active = false;
    gpio_set_dir_in_masked(DATA_MASK);

    int n = 0;
    absolute_time_t deadline = make_timeout_time_ms(2000);
    while (n < ROM_SIZE) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
                printf("\nupload timeout after %d bytes\n", n);
                rom_active = was_active;
                return;
            }
            continue;
        }
        rom_image[n++] = (uint8_t)c;
        deadline = make_timeout_time_ms(2000);
    }

    rom_active = was_active;
    printf("loaded %d bytes\n", n);
    printf("  reset vector → $%02X%02X\n",
           rom_image[0xFFFD - 0x8000], rom_image[0xFFFC - 0x8000]);
}

// ─── Command handling ───────────────────────────────────────────────────

static void show_help(void) {
    printf(
        "\nCommands:\n"
        "  c1 / c2 / c4   start PHI2 clock at 1 / 2 / 4 MHz\n"
        "  c500           start PHI2 at 500 kHz (use c<freq_khz> for any value)\n"
        "  cs             stop PHI2\n"
        "  r0             assert RESET (CPU halt)\n"
        "  r1             release RESET (CPU run)\n"
        "  a              sample address bus once\n"
        "  am             monitor address bus continuously (5x/sec)\n"
        "  as             stop monitor\n"
        "  rom            start ROM emulator (serves rom_image[] over $8000-$FFFF)\n"
        "  roms           stop ROM emulator (data bus → Hi-Z)\n"
        "  loadbin        upload a 32 KB ROM image over USB-CDC\n"
        "  watch <hex>    print data byte whenever bus address == <hex> (default $4000)\n"
        "  unwatch        stop watching\n"
        "  s              status\n"
        "  h / ?          this help\n\n"
    );
}

static void show_status(void) {
    printf("Status:\n");
    printf("  Monitor: %s\n", monitor_addr ? "ON" : "off");
    printf("  ROM emu: %s\n", rom_active   ? "ON" : "off");
    printf("  Watch:   %s (addr=$%04X)\n",
           watch_active ? "ON" : "off", watch_addr);
    printf("  RESET:   %s\n", (gpio_get_dir(PIN_RESET) == GPIO_OUT) ? "ASSERTED" : "released");
    printf("  Last addr: $%04X  data: $%02X\n", addr_read(), data_read());
    printf("  ROM[$8000..$8005] = %02X %02X %02X %02X %02X %02X\n",
           rom_image[0], rom_image[1], rom_image[2],
           rom_image[3], rom_image[4], rom_image[5]);
    printf("  Reset vector → $%02X%02X\n",
           rom_image[0xFFFD - 0x8000], rom_image[0xFFFC - 0x8000]);
}

static void handle_cmd(const char *line) {
    // Skip leading whitespace
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == 0) return;

    if (line[0] == 'c') {
        if (line[1] == 's') {
            phi2_stop();
        } else if (isdigit((unsigned char)line[1])) {
            int khz = atoi(line + 1);
            if (khz == 0) {
                printf("?? bad freq\n");
                return;
            }
            // c1 = 1 MHz, c2 = 2 MHz, c4 = 4 MHz, anything else = kHz
            uint32_t hz;
            if (khz < 100)  hz = (uint32_t)khz * 1000000;  // c1..c4 = MHz
            else            hz = (uint32_t)khz * 1000;     // c500 = 500 kHz
            phi2_start(hz);
        } else {
            printf("?? cs, c1, c2, c4, c500\n");
        }
    } else if (line[0] == 'r') {
        // Longer commands first so "rom"/"roms" aren't shadowed by "r0"/"r1".
        if (strncmp(line, "roms", 4) == 0) {
            rom_active = false;
            gpio_set_dir_in_masked(DATA_MASK);
            printf("ROM emu off\n");
        } else if (strncmp(line, "rom", 3) == 0) {
            rom_active = true;
            printf("ROM emu on (serving rom_image[] over $8000-$FFFF)\n");
        } else if (line[1] == '0') {
            reset_assert();
        } else if (line[1] == '1') {
            reset_release();
        } else {
            printf("?? r0, r1, rom, roms\n");
        }
    } else if (line[0] == 'a') {
        if (line[1] == 'm')      { monitor_addr = true;  printf("monitor on\n"); }
        else if (line[1] == 's') { monitor_addr = false; printf("monitor off\n"); }
        else {
            printf("addr=$%04X data=$%02X\n", addr_read(), data_read());
        }
    } else if (strncmp(line, "loadbin", 7) == 0) {
        cmd_loadbin();
    } else if (strncmp(line, "watch", 5) == 0) {
        const char *p = line + 5;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p) {
            unsigned int addr = (unsigned int)strtoul(p, NULL, 16);
            watch_addr = (uint16_t)(addr & 0xFFFFu);
        }
        watch_active       = true;
        watch_have_printed = false;
        watch_pending      = false;
        printf("watch on, addr=$%04X\n", watch_addr);
    } else if (strncmp(line, "unwatch", 7) == 0) {
        watch_active = false;
        printf("watch off\n");
    } else if (line[0] == 's') {
        show_status();
    } else if (line[0] == 'h' || line[0] == '?') {
        show_help();
    } else {
        printf("?? type 'h' for help\n");
    }
}

// ─── Main loop ──────────────────────────────────────────────────────────

static void print_banner(void) {
    printf("\n=== pico-rom-test — bring-up firmware ===\n");
    printf("65C02 + Pico-as-ROM + HM62256LP on 3.3 V\n");
    show_help();
    printf("ready> ");
    stdio_flush();
}

int main(void) {
    stdio_init_all();
    pins_init();
    rom_image_init();

    // Onboard LED — gives a visual heartbeat so you can tell the firmware
    // is actually running even if USB is misbehaving.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Wait (with a heartbeat) until the host opens the USB-CDC port.
    // This means screen/minicom always sees the banner.
    absolute_time_t led_toggle = get_absolute_time();
    bool led_on = false;
    while (!stdio_usb_connected()) {
        if (absolute_time_diff_us(get_absolute_time(), led_toggle) <= 0) {
            led_on = !led_on;
            gpio_put(PICO_DEFAULT_LED_PIN, led_on);
            led_toggle = make_timeout_time_ms(250);  // fast blink = waiting
        }
        tight_loop_contents();
    }

    // Connected — print banner. Brief delay so the host terminal is ready.
    sleep_ms(200);
    print_banner();

    char line[64];
    int  pos = 0;
    absolute_time_t next_monitor = get_absolute_time();
    absolute_time_t next_blink   = get_absolute_time();
    gpio_put(PICO_DEFAULT_LED_PIN, 1);  // solid on = connected & running

    while (true) {
        // ROM emulation runs every iteration (~MHz polling rate)
        rom_task();

        // Slow heartbeat blink (1 Hz) so you can see firmware is alive
        if (absolute_time_diff_us(get_absolute_time(), next_blink) <= 0) {
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
            next_blink = make_timeout_time_ms(500);
        }

        // Periodic address monitor (5 Hz)
        if (monitor_addr && absolute_time_diff_us(get_absolute_time(), next_monitor) <= 0) {
            uint16_t addr = addr_read();
            uint8_t  data = data_read();
            bool     a15  = gpio_get(PIN_A15);
            printf("[mon] addr=$%04X data=$%02X a15=%d\n", addr, data, a15);
            next_monitor = make_timeout_time_ms(200);
        }

        // Watch: print only on change so a tight loop doesn't flood the
        // terminal. Snapshot the volatile so rom_task can overwrite freely.
        if (watch_pending) {
            uint8_t d = watch_pending_data;
            watch_pending = false;
            if (!watch_have_printed || d != watch_last_printed) {
                printf("[$%04X = $%02X]\n", watch_addr, d);
                watch_last_printed = d;
                watch_have_printed = true;
            }
        }

        // Command input (non-blocking)
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) continue;

        // Echo back so the user can see what they're typing in screen/minicom
        if (c == '\r' || c == '\n') {
            putchar('\r');
            putchar('\n');
        } else if (c == 0x7F || c == 0x08) {
            // Backspace
            if (pos > 0) {
                pos--;
                printf("\b \b");
            }
            stdio_flush();
            continue;
        } else {
            putchar((char)c);
        }
        stdio_flush();

        if (c == '\r' || c == '\n') {
            line[pos] = 0;
            if (pos > 0) handle_cmd(line);
            pos = 0;
            printf("ready> ");
            stdio_flush();
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)c;
        }
    }
}
