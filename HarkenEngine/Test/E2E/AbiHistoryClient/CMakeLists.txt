cmake_minimum_required(VERSION 3.25)
project(HarkenAbiHistoryClient VERSION 1.0.0 LANGUAGES CXX)

if(NOT DEFINED HARKEN_PUBLIC_INCLUDE_DIR)
    message(FATAL_ERROR "HARKEN_PUBLIC_INCLUDE_DIR is required")
endif()
if(NOT DEFINED HARKEN_BINDING_PIN_DIR)
    message(FATAL_ERROR "HARKEN_BINDING_PIN_DIR is required")
endif()

add_executable(harken_abi_history_client AbiHistoryClient.cpp)
target_compile_features(harken_abi_history_client PRIVATE cxx_std_17)
target_include_directories(
    harken_abi_history_client PRIVATE
    "${HARKEN_PUBLIC_INCLUDE_DIR}"
    "${HARKEN_BINDING_PIN_DIR}")
if(MSVC)
    target_compile_options(harken_abi_history_client PRIVATE /utf-8)
endif()
