# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only build the components that `main` actually depends on. Together with an
# accurate REQUIRES list in main/CMakeLists.txt this keeps Wi-Fi, lwIP,
# esp_netif and wpa_supplicant out of the firmware image.
idf_build_set_property(MINIMAL_BUILD ON)

project(esp32-c3-idf-RMT)
