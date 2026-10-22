add_library(harken_studio_pack_build STATIC
    src/BindingMaterialization.cpp
    src/Curve.cpp
    src/World.cpp
    src/WorldCodec.cpp
    src/WorldArtifact.cpp
    src/PackImage.cpp
)
target_include_directories(harken_studio_pack_build PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(harken_studio_pack_build PUBLIC
    harken_studio_types
    harken_buildinfo
)
target_compile_features(harken_studio_pack_build PUBLIC cxx_std_20)
