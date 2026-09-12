cmake_minimum_required(VERSION 3.23)

project(usdPointCloudAuthoring
    VERSION 0.1.0
    DESCRIPTION "OpenUSD point-cloud and usdLod authoring bridge"
    LANGUAGES CXX)

find_package(pxr CONFIG REQUIRED)

add_library(usdPointCloudAuthoring STATIC
    src/ArAssetRandomAccessSource.cpp
    src/GeneratedPayloadSet.cpp
    src/PointCloudCache.cpp
    src/PointCloudLayer.cpp
    src/PointCloudStreaming.cpp)
add_library(usdpointcloud::authoring ALIAS usdPointCloudAuthoring)
# Deprecated compatibility alias for the pre-rename target name. Remove in
# v0.3.0; see docs/compatibility/MIGRATION.md.
add_library(usdgeo::usd ALIAS usdPointCloudAuthoring)
set_target_properties(usdPointCloudAuthoring PROPERTIES
    EXPORT_NAME authoring
    POSITION_INDEPENDENT_CODE ON)

target_compile_features(usdPointCloudAuthoring PUBLIC cxx_std_17)
target_link_libraries(usdPointCloudAuthoring
    PUBLIC
        usdgeo::core
        usdgeo::cache
        usdpointcloud::core
        usdpointcloud::tiling
        ar
        usdGeom
        usdLod)
target_include_directories(usdPointCloudAuthoring
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")

if(MSVC)
    target_compile_definitions(usdPointCloudAuthoring PUBLIC NOMINMAX)
    target_compile_options(usdPointCloudAuthoring PRIVATE /utf-8)
endif()

include(GNUInstallDirs)
install(TARGETS usdPointCloudAuthoring
    EXPORT usdPointCloudAuthoringTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(USDGEO_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(USDGEO_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
