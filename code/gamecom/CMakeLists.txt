cmake_minimum_required(VERSION 3.13)

# RP2350 Raspberry Pi Pico 2
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in SDK
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

set(PROJECT gamecom)

project(${PROJECT})

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_compile_options( -Wall )

# For boards with crystals which take a bit longer to stablize.
add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

# Add pico_hdmi library
add_subdirectory(../pico_hdmi)

add_executable(${PROJECT}
    main.c
)

# Set to 720p.
target_compile_definitions(${PROJECT} PRIVATE VIDEO_MODE_1280x720)
target_compile_definitions(pico_hdmi PRIVATE VIDEO_MODE_1280x720)
pico_set_binary_type(${PROJECT} copy_to_ram)

pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/captureVideo.pio ${CMAKE_CURRENT_LIST_DIR}/frameStart.pio ${CMAKE_CURRENT_LIST_DIR}/frameStartNotifier.pio)


target_link_libraries(${PROJECT}
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_adc
    hardware_pio
    hardware_irq
    pico_hdmi
)

# Enable USB output, disable UART
pico_enable_stdio_usb(${PROJECT} 1)
pico_enable_stdio_uart(${PROJECT} 0)

pico_add_extra_outputs(${PROJECT})
