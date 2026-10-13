# Opt in only in a dedicated Unity test-native build.
function(add_harken_native_test_bridge)
    target_sources(harken_core_shared PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/NativeTestBridge.cpp")
endfunction()
cmake_language(DEFER CALL add_harken_native_test_bridge)
