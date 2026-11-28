# Only here to give the ESP-IDF build system an application to configure a
# bootloader for. What this project is built for is bootloader.bin.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aros_esp32p4_bootloader)
