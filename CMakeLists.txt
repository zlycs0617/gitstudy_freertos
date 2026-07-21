cmake_minimum_required(VERSION 3.20)

project(freertos_study C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(_freertos_candidates
    "${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-KernelV11.3.0"
    "${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-Kernel-11.3.0/FreeRTOS-Kernel-11.3.0"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/FreeRTOS-Kernel"
    "${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-Kernel")

foreach(_candidate IN LISTS _freertos_candidates)
    if(EXISTS "${_candidate}/CMakeLists.txt")
        set(_detected_freertos_kernel_path "${_candidate}")
        break()
    endif()
endforeach()

if(NOT DEFINED _detected_freertos_kernel_path)
    set(_detected_freertos_kernel_path "")
endif()

set(FREERTOS_KERNEL_PATH
    "${_detected_freertos_kernel_path}"
    CACHE PATH "Path to FreeRTOS-Kernel")

if(NOT EXISTS "${FREERTOS_KERNEL_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "FreeRTOS-Kernel was not found.\n"
        "Put the kernel in one of these locations:\n"
        "  FreeRTOS-KernelV11.3.0\n"
        "  FreeRTOS-Kernel-11.3.0/FreeRTOS-Kernel-11.3.0\n"
        "  third_party/FreeRTOS-Kernel\n"
        "  FreeRTOS-Kernel\n")
endif()

message(STATUS "Using FreeRTOS-Kernel: ${FREERTOS_KERNEL_PATH}")

add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_definitions(freertos_config INTERFACE
    projCOVERAGE_TEST=0)

set(FREERTOS_HEAP "4" CACHE STRING "FreeRTOS heap implementation")
set(FREERTOS_PORT "MSVC_MINGW" CACHE STRING "FreeRTOS Windows desktop port")
set_property(CACHE FREERTOS_HEAP PROPERTY STRINGS 1 2 3 4 5)
set_property(CACHE FREERTOS_PORT PROPERTY STRINGS MSVC_MINGW GCC_POSIX)

message(STATUS "Using FreeRTOS heap_${FREERTOS_HEAP}.c")
message(STATUS "Using FreeRTOS port: ${FREERTOS_PORT}")

add_subdirectory("${FREERTOS_KERNEL_PATH}" freertos_kernel_build)

add_executable(freertos_study
    app_assert.c
    main.c)

target_link_libraries(freertos_study PRIVATE
    freertos_kernel
    freertos_config)

if(MSVC)
    target_compile_options(freertos_study PRIVATE /W4)
    target_compile_definitions(freertos_study PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(freertos_study PRIVATE -Wall -Wextra -Wpedantic)
endif()
