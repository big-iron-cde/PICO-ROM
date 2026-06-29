# Pico-as-ROM Documentation

A Raspberry Pi Pico 2 acts as the **ROM and clock** for a real **W65C02S** CPU on a
breadboard, using a single 3.3 V logic level — no EPROM, no oscillator can, no level
shifters. The Pico holds a 32 KB ROM image in SRAM, serves it to the CPU's address bus
in real time, generates PHI2, and drives RESET. A host PC builds ROM images and controls
the Pico over a framed USB-serial **Hardware API**.

This site is the complete reference. For a quick overview and the condensed wiring, see
the [project README](https://github.com/big-iron-cde/PICO-ROM#readme).

```{mermaid}
flowchart LR
    CPU["W65C02S CPU"]
    Pico["Raspberry Pi Pico 2<br/>(ROM + clock + reset)"]
    RAM["HM62256 SRAM"]
    Host["Host PC<br/>(rom-builder)"]

    CPU <-->|"A0-A15 / D0-D7 bus"| Pico
    CPU <-->|"A0-A14 / D0-D7 bus"| RAM
    Pico -->|"PHI2 clock"| CPU
    Pico -->|"RESET (GP27)"| CPU
    Pico -->|"A15 = CE#"| RAM
    Host <-->|"USB-CDC Hardware API"| Pico
```

## Contents

```{toctree}
:maxdepth: 2
:caption: Hardware

hardware/memory-map
hardware/pinout
hardware/wiring
hardware/power
hardware/bring-up
hardware/troubleshooting
```

```{toctree}
:maxdepth: 2
:caption: Software

firmware
hardware-api
host-tools
```

## What this prototype can and cannot do

**Can:**

- Run the 65C02 from a Pico-hosted 32 KB ROM image (`$8000–$FFFF`).
- Build a ROM on a laptop, upload it over USB via the Hardware API.
- Auto-start clock, ROM, and reset on USB connect.
- Control reset, upload ROM, capture bus cycles (until `STP`), and query the address bus.
- Snoop a virtual print port (`$4000`) via `read` or `monitor`.

**Cannot (deliberately deferred for the prototype):**

- Fail-safe behavior when the Pico is unpowered.
- Hot-swap ROM updates via CPU tri-state (BE is held high).
- Robust noise immunity (no decoupling caps).
- Guaranteed RAM reliability (HM62256 is out of spec at 3.3 V).
