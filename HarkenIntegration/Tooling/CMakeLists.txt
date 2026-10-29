find_package(Qt6 REQUIRED COMPONENTS Core)
add_library(harken_integration_tooling STATIC UnityIntegrationService.cpp UnityIntegrationService.h
    UnityInstallation.cpp UnityInstallation.h)
set_target_properties(harken_integration_tooling PROPERTIES AUTOMOC ON)
target_include_directories(harken_integration_tooling PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(harken_integration_tooling PUBLIC Qt6::Core)
