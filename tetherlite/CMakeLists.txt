cmake_minimum_required(VERSION 3.24)
project(TetherLite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TETHERLITE_BUILD_TESTS "Build unit tests" ON)
option(TETHERLITE_ENABLE_ASAN "AddressSanitizer + UBSan" OFF)
option(TETHERLITE_ENABLE_TSAN "ThreadSanitizer" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(TETHERLITE_ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(TETHERLITE_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()

# Platform-independent protocol codec.
add_library(tl_rndis STATIC src/rndis.cpp)
target_include_directories(tl_rndis PUBLIC include)

if(TETHERLITE_BUILD_TESTS)
  enable_testing()
  add_executable(test_rndis tests/test_rndis.cpp)
  target_link_libraries(test_rndis PRIVATE tl_rndis)
  add_test(NAME rndis COMMAND test_rndis)
endif()

# The driver itself only makes sense on macOS (feth + BPF).
if(APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
  find_package(Threads REQUIRED)
  add_executable(tetherlite
    src/main.cpp src/bridge.cpp src/usb_device.cpp src/feth.cpp src/bpf.cpp)
  target_link_libraries(tetherlite PRIVATE tl_rndis PkgConfig::LIBUSB Threads::Threads)
else()
  message(STATUS "Not macOS: building codec and tests only")
endif()
