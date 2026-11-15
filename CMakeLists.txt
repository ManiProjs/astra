cmake_minimum_required(VERSION 3.25)

if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Apple platform architectures")
endif()

project(Astra
    VERSION 0.1.0
    DESCRIPTION "A modular, high-performance 3D game engine"
    LANGUAGES CXX
)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)
option(ASTRA_BUILD_EXAMPLES "Build the runnable Astra examples" ON)
option(ASTRA_ENABLE_PHYSICS "Build the Jolt-backed rigid-body physics module" ON)

if(NOT APPLE AND NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Astra supports macOS, Windows, and Linux build hosts; found ${CMAKE_SYSTEM_NAME}.")
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
    message(FATAL_ERROR "Astra requires an LLVM Clang-family compiler; found ${CMAKE_CXX_COMPILER_ID}. GCC is not supported.")
endif()

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64|x86_64|arm64;x86_64|x86_64;arm64)$")
    message(FATAL_ERROR "Unsupported macOS architecture selection: ${CMAKE_OSX_ARCHITECTURES}.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "Astra ${PROJECT_VERSION} project configuration")
message(STATUS "Target platform: ${CMAKE_SYSTEM_NAME}")
if(APPLE)
    message(STATUS "Apple architectures: ${CMAKE_OSX_ARCHITECTURES}")
else()
    message(STATUS "Target architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT)
    message(STATUS "Compiler frontend: ${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}")
endif()
message(STATUS "C++ standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "Generator: ${CMAKE_GENERATOR}")

add_library(AstraCore STATIC
    engine/core/src/allocator.cpp
    engine/core/src/config.cpp
    engine/core/src/log.cpp
    engine/core/src/thread_pool.cpp
)
add_library(Astra::Core ALIAS AstraCore)
target_include_directories(AstraCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/core/include>
)
target_compile_features(AstraCore PUBLIC cxx_std_23)
target_link_libraries(AstraCore PUBLIC Threads::Threads)
target_compile_options(AstraCore PRIVATE -Wall -Wextra -Wpedantic)

if(APPLE)
    set(ASTRA_WINDOW_SOURCE engine/platform/macos/window_macos.mm)
else()
    set(ASTRA_WINDOW_SOURCE engine/platform/src/window_unsupported.cpp)
endif()

add_library(AstraPlatform STATIC
    engine/platform/src/application.cpp
    engine/platform/src/filesystem.cpp
    engine/platform/src/system_info.cpp
    ${ASTRA_WINDOW_SOURCE}
)
add_library(Astra::Platform ALIAS AstraPlatform)
target_include_directories(AstraPlatform
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/platform/include>
)
target_compile_features(AstraPlatform PUBLIC cxx_std_23)
target_compile_options(AstraPlatform PRIVATE -Wall -Wextra -Wpedantic)
if(APPLE)
    target_link_libraries(AstraPlatform PUBLIC "-framework AppKit" "-framework QuartzCore")
    target_compile_options(AstraPlatform PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

add_library(AstraRHI INTERFACE)
add_library(Astra::RHI ALIAS AstraRHI)
target_include_directories(AstraRHI INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/rhi/include>
)
target_compile_features(AstraRHI INTERFACE cxx_std_23)

if(APPLE)
    find_program(ASTRA_XCRUN_EXECUTABLE xcrun REQUIRED)
    set(ASTRA_METAL_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/metal_shaders")
    set(ASTRA_METAL_SHADER_AIR "${ASTRA_METAL_SHADER_DIR}/scene.air")
    set(ASTRA_METAL_IBL_SHADER_AIR "${ASTRA_METAL_SHADER_DIR}/ibl.air")
    set(ASTRA_METAL_VIRTUAL_GEOMETRY_ICB_SHADER_AIR "${ASTRA_METAL_SHADER_DIR}/virtual_geometry_icb.air")
    set(ASTRA_METAL_SHADER_LIBRARY "${ASTRA_METAL_SHADER_DIR}/scene.metallib")
    add_custom_command(
        OUTPUT "${ASTRA_METAL_SHADER_LIBRARY}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ASTRA_METAL_SHADER_DIR}"
        COMMAND "${ASTRA_XCRUN_EXECUTABLE}" -sdk macosx metal -c
                "${CMAKE_CURRENT_SOURCE_DIR}/backends/metal/shaders/scene.metal"
                -o "${ASTRA_METAL_SHADER_AIR}"
        COMMAND "${ASTRA_XCRUN_EXECUTABLE}" -sdk macosx metal -c
                "${CMAKE_CURRENT_SOURCE_DIR}/backends/metal/shaders/ibl.metal"
                -o "${ASTRA_METAL_IBL_SHADER_AIR}"
        COMMAND "${ASTRA_XCRUN_EXECUTABLE}" -sdk macosx metal -c
                "${CMAKE_CURRENT_SOURCE_DIR}/backends/metal/shaders/virtual_geometry_icb.metal"
                -o "${ASTRA_METAL_VIRTUAL_GEOMETRY_ICB_SHADER_AIR}"
        COMMAND "${ASTRA_XCRUN_EXECUTABLE}" -sdk macosx metallib
                "${ASTRA_METAL_SHADER_AIR}" "${ASTRA_METAL_IBL_SHADER_AIR}"
                "${ASTRA_METAL_VIRTUAL_GEOMETRY_ICB_SHADER_AIR}"
                -o "${ASTRA_METAL_SHADER_LIBRARY}"
        DEPENDS backends/metal/shaders/scene.metal backends/metal/shaders/ibl.metal
                backends/metal/shaders/virtual_geometry_icb.metal
        COMMENT "Compiling Astra Metal shaders"
        VERBATIM
    )
    add_custom_target(AstraMetalShaders DEPENDS "${ASTRA_METAL_SHADER_LIBRARY}")

    add_library(AstraMetal STATIC backends/metal/src/metal_device.mm)
    add_dependencies(AstraMetal AstraMetalShaders)
    target_include_directories(AstraMetal
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends/metal/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine/rhi/include
                ${CMAKE_CURRENT_SOURCE_DIR}/engine/renderer/include
    )
    target_compile_features(AstraMetal PUBLIC cxx_std_23)
    target_compile_options(AstraMetal PRIVATE -Wall -Wextra -Wpedantic -fobjc-arc)
    target_compile_definitions(AstraMetal PRIVATE
        ASTRA_METALLIB_PATH="${ASTRA_METAL_SHADER_LIBRARY}"
    )
    target_link_libraries(AstraMetal PUBLIC Astra::RHI "-framework Metal" "-framework QuartzCore")
endif()

add_library(AstraRenderer STATIC engine/renderer/src/renderer.cpp)
add_library(Astra::Renderer ALIAS AstraRenderer)
target_include_directories(AstraRenderer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/renderer/include>
)
target_compile_features(AstraRenderer PUBLIC cxx_std_23)
target_compile_options(AstraRenderer PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(AstraRenderer PUBLIC Astra::Platform Astra::RHI)
if(APPLE)
    target_compile_definitions(AstraRenderer PRIVATE ASTRA_HAS_METAL=1)
    target_link_libraries(AstraRenderer PRIVATE AstraMetal)
endif()

add_library(AstraGame STATIC engine/game/src/game.cpp)
add_library(Astra::Game ALIAS AstraGame)
target_include_directories(AstraGame PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/game/include>
)
target_compile_features(AstraGame PUBLIC cxx_std_23)
target_compile_options(AstraGame PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(AstraGame PUBLIC Astra::Platform Astra::Renderer)

add_library(AstraWorld INTERFACE)
add_library(Astra::World ALIAS AstraWorld)
target_include_directories(AstraWorld INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/world/include>
)
target_compile_features(AstraWorld INTERFACE cxx_std_23)
target_link_libraries(AstraWorld INTERFACE Astra::Core)

if(ASTRA_ENABLE_PHYSICS)
    # Jolt is MIT-licensed and supplies the production rigid-body, collision,
    # constraint, character and query implementation behind Astra's facade.
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "Disable Jolt LTO for embedded builds" FORCE)
    set(ENABLE_INSTALL OFF CACHE BOOL "Do not install the embedded Jolt dependency" FORCE)
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "Use Astra's warning policy" FORCE)
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "Keep Jolt debug renderer out of runtime" FORCE)
    set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "Keep Jolt profiler out of runtime" FORCE)
    set(JPH_USE_MTL OFF CACHE BOOL "Use CPU physics implementation" FORCE)
    set(JPH_USE_DX12 OFF CACHE BOOL "Use CPU physics implementation" FORCE)
    set(JPH_USE_VK OFF CACHE BOOL "Use CPU physics implementation" FORCE)
    FetchContent_Declare(JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG e77f175595e64cb44218cc9d9d56fc365ad0e36a # v5.6.0
        SOURCE_SUBDIR Build
    )
    FetchContent_MakeAvailable(JoltPhysics)

    add_library(AstraPhysics STATIC engine/physics/src/physics_world.cpp)
    add_library(Astra::Physics ALIAS AstraPhysics)
    target_include_directories(AstraPhysics PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/physics/include>
    )
    target_compile_features(AstraPhysics PUBLIC cxx_std_23)
    target_compile_options(AstraPhysics PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(AstraPhysics PUBLIC Astra::RHI Jolt Threads::Threads)
endif()

add_library(AstraGeometry INTERFACE)
add_library(Astra::Geometry ALIAS AstraGeometry)
target_include_directories(AstraGeometry INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/geometry/include>
)
target_compile_features(AstraGeometry INTERFACE cxx_std_23)

if(ASTRA_BUILD_EXAMPLES)
    add_executable(astra_virtual_geometry_streaming examples/virtual_geometry_streaming/main.cpp)
    target_link_libraries(astra_virtual_geometry_streaming PRIVATE Astra::Geometry)
    target_compile_options(astra_virtual_geometry_streaming PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(APPLE)
    set(ASTRA_IMAGE_DECODER_SOURCE engine/assets/src/image_decode_macos.mm)
else()
    set(ASTRA_IMAGE_DECODER_SOURCE engine/assets/src/image_decode_unsupported.cpp)
endif()
add_library(AstraAssets STATIC
    engine/assets/src/gltf.cpp
    engine/assets/src/hdr.cpp
    ${ASTRA_IMAGE_DECODER_SOURCE}
)
add_library(Astra::Assets ALIAS AstraAssets)
target_include_directories(AstraAssets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/assets/include>
)
target_compile_features(AstraAssets PUBLIC cxx_std_23)
target_compile_options(AstraAssets PRIVATE -Wall -Wextra -Wpedantic)
if(APPLE)
    target_compile_options(AstraAssets PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(AstraAssets PRIVATE "-framework Foundation" "-framework CoreGraphics"
        "-framework ImageIO")
endif()

if(BUILD_TESTING)
    function(astra_enable_test_assertions target)
        # Release configurations define NDEBUG globally; keep test assertions active.
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endfunction()

    add_executable(astra_runtime_foundation_tests
        tests/runtime_foundation_tests.cpp
    )
    target_link_libraries(astra_runtime_foundation_tests
        PRIVATE
            Astra::Core
            Astra::Platform
    )
    target_compile_options(astra_runtime_foundation_tests PRIVATE -Wall -Wextra -Wpedantic)
    astra_enable_test_assertions(astra_runtime_foundation_tests)
    add_test(NAME astra.runtime_foundations COMMAND astra_runtime_foundation_tests)

    add_executable(astra_platform_runtime_tests
        tests/platform_runtime_tests.cpp
    )
    target_link_libraries(astra_platform_runtime_tests PRIVATE Astra::Platform)
    target_compile_options(astra_platform_runtime_tests PRIVATE -Wall -Wextra -Wpedantic)
    astra_enable_test_assertions(astra_platform_runtime_tests)
    add_test(NAME astra.platform_runtime COMMAND astra_platform_runtime_tests)

    add_executable(astra_window_api_tests tests/window_api_tests.cpp)
    target_link_libraries(astra_window_api_tests PRIVATE Astra::Platform)
    target_compile_options(astra_window_api_tests PRIVATE -Wall -Wextra -Wpedantic)
    astra_enable_test_assertions(astra_window_api_tests)
    add_test(NAME astra.window_api COMMAND astra_window_api_tests)

    add_executable(astra_game_api_tests tests/game_api_tests.cpp)
    target_link_libraries(astra_game_api_tests PRIVATE Astra::Game)
    target_compile_options(astra_game_api_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    astra_enable_test_assertions(astra_game_api_tests)
    add_test(NAME astra.game_api COMMAND astra_game_api_tests)

    add_executable(astra_world_module_tests tests/world_module_tests.cpp)
    target_link_libraries(astra_world_module_tests PRIVATE Astra::World)
    target_compile_options(astra_world_module_tests PRIVATE -Wall -Wextra -Wpedantic)
    astra_enable_test_assertions(astra_world_module_tests)
    add_test(NAME astra.world_modules COMMAND astra_world_module_tests)

    if(ASTRA_ENABLE_PHYSICS)
        add_executable(astra_physics_tests tests/physics_tests.cpp)
        target_link_libraries(astra_physics_tests PRIVATE Astra::Physics)
        target_compile_options(astra_physics_tests PRIVATE -Wall -Wextra -Wpedantic)
        astra_enable_test_assertions(astra_physics_tests)
        add_test(NAME astra.physics COMMAND astra_physics_tests)
    endif()

    add_executable(astra_virtual_geometry_tests
        engine/geometry/tests/virtual_geometry_tests.cpp
    )
    target_link_libraries(astra_virtual_geometry_tests PRIVATE Astra::Geometry)
    target_compile_options(astra_virtual_geometry_tests PRIVATE -Wall -Wextra -Wpedantic)
    astra_enable_test_assertions(astra_virtual_geometry_tests)
    add_test(NAME astra.virtual_geometry COMMAND astra_virtual_geometry_tests)

    add_executable(astra_rhi_header_tests tests/rhi_header_tests.cpp)
    target_link_libraries(astra_rhi_header_tests PRIVATE Astra::RHI)
    target_compile_options(astra_rhi_header_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    astra_enable_test_assertions(astra_rhi_header_tests)
    add_test(NAME astra.rhi_headers COMMAND astra_rhi_header_tests)

    add_executable(astra_temporal_tests tests/temporal_tests.cpp)
    target_link_libraries(astra_temporal_tests PRIVATE Astra::Renderer)
    target_compile_options(astra_temporal_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    astra_enable_test_assertions(astra_temporal_tests)
    add_test(NAME astra.temporal COMMAND astra_temporal_tests)

    add_executable(astra_oit_tests tests/oit_tests.cpp)
    target_link_libraries(astra_oit_tests PRIVATE Astra::Renderer)
    target_compile_options(astra_oit_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    astra_enable_test_assertions(astra_oit_tests)
    add_test(NAME astra.oit COMMAND astra_oit_tests)

    add_executable(astra_asset_animation_tests engine/assets/tests/animation_tests.cpp)
    target_link_libraries(astra_asset_animation_tests PRIVATE Astra::Assets)
    target_compile_options(astra_asset_animation_tests PRIVATE -Wall -Wextra -Wpedantic)
    astra_enable_test_assertions(astra_asset_animation_tests)
    add_test(NAME astra.asset_animation COMMAND astra_asset_animation_tests)

    add_executable(astra_hdr_image_tests engine/assets/tests/hdr_image_tests.cpp)
    target_link_libraries(astra_hdr_image_tests PRIVATE Astra::Assets)
    target_compile_options(astra_hdr_image_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    astra_enable_test_assertions(astra_hdr_image_tests)
    add_test(NAME astra.asset_hdr_image COMMAND astra_hdr_image_tests)

    add_executable(astra_gltf_validation_tests engine/assets/tests/gltf_validation_tests.cpp)
    target_link_libraries(astra_gltf_validation_tests PRIVATE Astra::Assets)
    target_compile_options(astra_gltf_validation_tests PRIVATE -Wall -Wextra -Wpedantic)
    astra_enable_test_assertions(astra_gltf_validation_tests)
    add_test(NAME astra.gltf_validation COMMAND astra_gltf_validation_tests)

    if(APPLE)
        add_executable(astra_morph_gpu_tests tests/metal_morph_tests.mm)
        add_dependencies(astra_morph_gpu_tests AstraMetalShaders)
        target_compile_definitions(astra_morph_gpu_tests PRIVATE
            ASTRA_METALLIB_PATH="${ASTRA_METAL_SHADER_LIBRARY}"
        )
        target_compile_options(astra_morph_gpu_tests PRIVATE -Wall -Wextra -Wpedantic -fobjc-arc)
        target_link_libraries(astra_morph_gpu_tests PRIVATE AstraMetal "-framework Foundation"
            "-framework Metal" "-framework QuartzCore")
        astra_enable_test_assertions(astra_morph_gpu_tests)
        add_test(NAME astra.morph_gpu COMMAND astra_morph_gpu_tests)
        set_tests_properties(astra.morph_gpu PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    add_executable(astra_gltf_asset_smoke engine/assets/tests/gltf_asset_smoke.cpp)
    target_link_libraries(astra_gltf_asset_smoke PRIVATE Astra::Assets)
    target_compile_definitions(astra_gltf_asset_smoke PRIVATE
        ASTRA_CESIUMMAN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/characters/CesiumMan.glb"
        ASTRA_PLAYER_ASSET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/characters/AstraPlayer-runtime.glb"
        ASTRA_BOAR_ASSET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/characters/Boarbound.glb"
        ASTRA_BOAR_HUMAN_ASSET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/characters/Boarbound-Human.glb"
        ASTRA_ANIMATED_BOAR_ASSET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/characters/AstraBoar.glb"
        ASTRA_NATURE_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets/nature/runtime"
        ASTRA_TERRAIN_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets/terrain/forest_ground_04"
    )
    target_compile_options(astra_gltf_asset_smoke PRIVATE -Wall -Wextra -Wpedantic)
    astra_enable_test_assertions(astra_gltf_asset_smoke)
    add_test(NAME astra.gltf_asset_smoke COMMAND astra_gltf_asset_smoke)
endif()

if(ASTRA_BUILD_EXAMPLES)
    add_executable(astra_example_core examples/core_foundations.cpp)
    target_link_libraries(astra_example_core PRIVATE Astra::Core)
    target_compile_options(astra_example_core PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(astra_example_platform examples/platform_foundations.cpp)
    target_link_libraries(astra_example_platform PRIVATE Astra::Core Astra::Platform)
    target_compile_options(astra_example_platform PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(astra_sandbox examples/sandbox/main.cpp)
    target_link_libraries(astra_sandbox PRIVATE Astra::Core Astra::Platform)
    target_compile_options(astra_sandbox PRIVATE -Wall -Wextra -Wpedantic)

    if(ASTRA_ENABLE_PHYSICS)
        add_executable(astra_physics_sandbox examples/physics/main.cpp)
        target_link_libraries(astra_physics_sandbox PRIVATE Astra::Physics)
        target_compile_options(astra_physics_sandbox PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(APPLE)
        add_executable(astra_windowed_3d_scene examples/windowed_3d_scene/main.cpp)
        target_link_libraries(astra_windowed_3d_scene
            PRIVATE Astra::Core Astra::Game
        )
        target_compile_options(astra_windowed_3d_scene PRIVATE -Wall -Wextra -Wpedantic)

        add_executable(astra_character_asset_inspection examples/boar_transformation/main.cpp)
        target_link_libraries(astra_character_asset_inspection PRIVATE Astra::Core Astra::Game Astra::Assets)
        target_compile_definitions(astra_character_asset_inspection PRIVATE
            ASTRA_BOAR_ASSET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/characters/AstraBoar.glb"
            ASTRA_BOAR_HUMAN_ASSET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/characters/Boarbound-Human.glb"
        )
        target_compile_options(astra_character_asset_inspection PRIVATE -Wall -Wextra -Wpedantic)

        add_executable(astra_oit_demo examples/oit/main.cpp)
        target_link_libraries(astra_oit_demo PRIVATE Astra::Core Astra::Game)
        target_compile_options(astra_oit_demo PRIVATE -Wall -Wextra -Wpedantic)

        add_executable(astra_ibl_demo examples/ibl/main.cpp)
        target_link_libraries(astra_ibl_demo PRIVATE Astra::Core Astra::Game Astra::Assets)
        target_compile_options(astra_ibl_demo PRIVATE -Wall -Wextra -Wpedantic)

        add_executable(astra_open_world examples/open_world/main.cpp)
        target_link_libraries(astra_open_world PRIVATE Astra::Core Astra::Game Astra::World Astra::Assets Astra::Geometry)
        if(ASTRA_ENABLE_PHYSICS)
            target_link_libraries(astra_open_world PRIVATE Astra::Physics)
            target_compile_definitions(astra_open_world PRIVATE ASTRA_HAS_PHYSICS=1)
        endif()
        target_compile_definitions(astra_open_world PRIVATE
            ASTRA_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets"
        )
        target_compile_options(astra_open_world PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(BUILD_TESTING)
        add_test(NAME astra.example.core COMMAND astra_example_core)
        add_test(NAME astra.example.platform COMMAND astra_example_platform)
        add_test(NAME astra.example.sandbox COMMAND astra_sandbox)
    endif()
endif()

message(STATUS "Astra Core, Platform, RHI, Renderer, Game, Assets, Geometry, and World targets are enabled.")
