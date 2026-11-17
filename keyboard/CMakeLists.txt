cmake_minimum_required(VERSION 3.13)

# Set PICO_SDK_PATH in the environment, or pass -DPICO_SDK_PATH=...
# For a Pico 2 (RP2350) add -DPICO_BOARD=pico2
include(pico_sdk_import.cmake)

project(t1k_keyboard C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(t1k_keyboard src/main.c src/usb_descriptors.c)
target_include_directories(t1k_keyboard PRIVATE src)
target_compile_options(t1k_keyboard PRIVATE -Wall -Wextra)
target_link_libraries(t1k_keyboard PRIVATE pico_stdlib pico_unique_id tinyusb_device)
pico_add_extra_outputs(t1k_keyboard)
