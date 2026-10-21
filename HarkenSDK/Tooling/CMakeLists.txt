find_package(Qt6 REQUIRED COMPONENTS Core)
add_library(harken_sdk_integration STATIC UnityIntegrationService.cpp UnityIntegrationService.h
    UnityInstallation.cpp UnityInstallation.h)
set_target_properties(harken_sdk_integration PROPERTIES AUTOMOC ON)
target_include_directories(harken_sdk_integration PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(harken_sdk_integration PUBLIC Qt6::Core)
