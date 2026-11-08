# Entry point for CMakePresets.json: firmware/ (the ESP32 / ESP8266 relay lives in HclX/wyzesense_relay).
cmake_minimum_required(VERSION 3.20)
project(openwyzesense-root NONE)
enable_testing()
add_subdirectory(firmware)
