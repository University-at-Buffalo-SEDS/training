cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME datalogger)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Keep application and middleware ThreadX layouts consistent.
target_compile_definitions(stm32cubemx INTERFACE TX_ENABLE_STACK_CHECKING)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    
    ${CMAKE_SOURCE_DIR}/Core/Src/flash_thread.c
    ${CMAKE_SOURCE_DIR}/Drivers/flash/w25q32jv.c
    Core/Src/heartbeat_thread.c
    Core/Src/spi_bus.c
    Core/Src/sensors.c
    Core/Src/records.c
    Core/Src/acquisition.c
    Core/Src/sensor_thread.c
    Core/Src/sensor_live.c
    Core/Src/processing_thread.c
    Core/Src/usb_thread.c
    Core/Src/usb_rx_thread.c
    Core/Src/usb_service.c
    Core/Src/usb_protocol.c
    Core/Src/usb_console.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_SOURCE_DIR}/Drivers/flash
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# ST USBX controller adapter from X-CUBE-AZRTOS-G4 2.0.0.
set(USBX_DCD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/usbx/common/usbx_stm32_device_controllers")
file(GLOB USBX_DCD_SOURCES CONFIGURE_DEPENDS "${USBX_DCD_DIR}/*.c")
target_include_directories(USBX PUBLIC "${USBX_DCD_DIR}")
target_sources(USBX PRIVATE ${USBX_DCD_SOURCES})
