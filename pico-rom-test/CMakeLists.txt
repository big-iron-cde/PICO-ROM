cmake_minimum_required(VERSION 3.13)

# IMPORTANT: PICO_BOARD must be set BEFORE include(pico_sdk_import.cmake)
# Pico 1 (RP2040).
set(PICO_BOARD pico CACHE STRING "Board type")

# Read PICO_SDK_PATH from environment
include($ENV{PICO_SDK_PATH}\\external\\pico_sdk_import.cmake)

project(pico-rom-test C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico-rom-test
    main.c
    protocol.c
    hardware_api.c
)

target_link_libraries(pico-rom-test
    pico_stdlib
    hardware_gpio
    hardware_clocks
    hardware_sync
    pico_cyw43_arch_none
)

pico_add_extra_outputs(pico-rom-test)

pico_enable_stdio_usb(pico-rom-test 1)
pico_enable_stdio_uart(pico-rom-test 0)
