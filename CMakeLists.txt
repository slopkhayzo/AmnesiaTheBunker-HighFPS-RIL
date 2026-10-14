cmake_minimum_required(VERSION 3.24)

project(
    AmnesiaTheBunkerFpsFix
    VERSION 1.0.0
    DESCRIPTION "High-frame-rate presentation fix for Amnesia: The Bunker"
    LANGUAGES C CXX
)

include(CTest)

set(ATB_GAME_EXE "" CACHE FILEPATH
    "Optional AmnesiaTheBunker.exe path for the read-only installed-target test")
option(ATB_BUILD_DEV_LAUNCHER
    "Build the non-release x64 development launcher/injector" ON)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "AmnesiaTheBunker.FPSFix supports Windows/MSVC only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Configure an x64 build environment; AmnesiaTheBunker.exe is a 64-bit process")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)
set_property(TARGET minhook PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_library(AmnesiaTheBunkerFpsFix MODULE
    src/bootstrap.cpp
    src/camera_interpolation.cpp
    src/camera_runtime.cpp
    src/camera_trace.cpp
    src/config.cpp
    src/dllmain.cpp
    src/frame_cadence.cpp
    src/frame_pacing.cpp
    src/frame_trace.cpp
    src/guarded_memory.cpp
    src/gui_cursor.cpp
    src/hands_trace.cpp
    src/hands_pose_interpolation.cpp
    src/logger.cpp
    src/mouse_trace.cpp
    src/particle_runtime.cpp
    src/particle_interpolation.cpp
    src/presentation_clock.cpp
    src/presentation_limiter.cpp
    src/runtime_mapping.cpp
    src/sha256.cpp
    src/target_mapping.cpp
    src/telemetry.cpp
    src/world_runtime.cpp
)

target_compile_features(AmnesiaTheBunkerFpsFix PRIVATE cxx_std_20)
target_compile_definitions(AmnesiaTheBunkerFpsFix PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_compile_options(AmnesiaTheBunkerFpsFix PRIVATE
    /W4
    /WX
    /permissive-
    /utf-8
    /Zc:preprocessor
)
target_link_options(AmnesiaTheBunkerFpsFix PRIVATE
    /DYNAMICBASE
    /NXCOMPAT
    /GUARD:CF
    /INCREMENTAL:NO
)
target_link_libraries(AmnesiaTheBunkerFpsFix PRIVATE minhook bcrypt)
set_property(TARGET AmnesiaTheBunkerFpsFix PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
set_target_properties(AmnesiaTheBunkerFpsFix PROPERTIES
    OUTPUT_NAME "AmnesiaTheBunker.FPSFix"
    PREFIX ""
    SUFFIX ".asi"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(ATB_BUILD_DEV_LAUNCHER)
    add_executable(atb_dev_launcher tools/dev_launcher.cpp)
    target_compile_features(atb_dev_launcher PRIVATE cxx_std_20)
    target_compile_definitions(atb_dev_launcher PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(atb_dev_launcher PRIVATE
        /W4 /WX /permissive- /utf-8)
    set_property(TARGET atb_dev_launcher PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_target_properties(atb_dev_launcher PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

add_custom_command(TARGET AmnesiaTheBunkerFpsFix POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.ini"
            "$<TARGET_FILE_DIR:AmnesiaTheBunkerFpsFix>/AmnesiaTheBunker.FPSFix.ini"
    VERBATIM
)

add_custom_target(stage_telemetry_profile
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.telemetry.ini"
            "$<TARGET_FILE_DIR:AmnesiaTheBunkerFpsFix>/AmnesiaTheBunker.FPSFix.ini"
    DEPENDS AmnesiaTheBunkerFpsFix
    COMMENT "Staging the opt-in, telemetry-only development profile"
    VERBATIM
)

install(TARGETS AmnesiaTheBunkerFpsFix LIBRARY DESTINATION . RUNTIME DESTINATION .)
install(FILES config/AmnesiaTheBunker.FPSFix.ini DESTINATION .)

# Release archives ship packaging/AmnesiaTheBunker.FPSFix.ini, with every
# validated feature on. config/AmnesiaTheBunker.FPSFix.ini is the development
# default, with everything off, and stays beside the built plugin.
set(ATB_PACKAGE_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/package.cmake")
set(ATB_ULTIMATE_ASI_LOADER_DLL "" CACHE FILEPATH
    "Optional x64 Ultimate ASI Loader version.dll used for the loader-bundled package")
add_custom_target(package_plugin_only
    COMMAND "${CMAKE_COMMAND}"
            -DMODE=plugin-only
            -DVERSION=${PROJECT_VERSION}
            -DPLUGIN_FILE=$<TARGET_FILE:AmnesiaTheBunkerFpsFix>
            -DINI_FILE=${CMAKE_SOURCE_DIR}/packaging/AmnesiaTheBunker.FPSFix.ini
            -DREADME_FILE=${CMAKE_SOURCE_DIR}/packaging/README.txt
            -DMINHOOK_LICENSE_FILE=${CMAKE_SOURCE_DIR}/third_party/minhook/LICENSE.txt
            -DOUTPUT_DIR=${CMAKE_SOURCE_DIR}/dist/release/AmnesiaTheBunker.FPSFix-${PROJECT_VERSION}-plugin-only
            -DARCHIVE_FILE=${CMAKE_SOURCE_DIR}/dist/AmnesiaTheBunker.FPSFix-${PROJECT_VERSION}-plugin-only.zip
            -P "${ATB_PACKAGE_SCRIPT}"
    DEPENDS AmnesiaTheBunkerFpsFix
    VERBATIM
)

if(ATB_ULTIMATE_ASI_LOADER_DLL AND EXISTS "${ATB_ULTIMATE_ASI_LOADER_DLL}")
    add_custom_target(package_loader_bundled
        COMMAND "${CMAKE_COMMAND}"
                -DMODE=loader-bundled
                -DVERSION=${PROJECT_VERSION}
                -DPLUGIN_FILE=$<TARGET_FILE:AmnesiaTheBunkerFpsFix>
                -DINI_FILE=${CMAKE_SOURCE_DIR}/packaging/AmnesiaTheBunker.FPSFix.ini
                -DREADME_FILE=${CMAKE_SOURCE_DIR}/packaging/README-loader.txt
                -DMINHOOK_LICENSE_FILE=${CMAKE_SOURCE_DIR}/third_party/minhook/LICENSE.txt
                -DLOADER_FILE=${ATB_ULTIMATE_ASI_LOADER_DLL}
                -DLOADER_LICENSE_FILE=${CMAKE_SOURCE_DIR}/third_party/ultimate-asi-loader/LICENSE.txt
                -DOUTPUT_DIR=${CMAKE_SOURCE_DIR}/dist/release/AmnesiaTheBunker.FPSFix-${PROJECT_VERSION}-with-Ultimate-ASI-Loader
                -DARCHIVE_FILE=${CMAKE_SOURCE_DIR}/dist/AmnesiaTheBunker.FPSFix-${PROJECT_VERSION}-with-Ultimate-ASI-Loader.zip
                -P "${ATB_PACKAGE_SCRIPT}"
        DEPENDS AmnesiaTheBunkerFpsFix
        VERBATIM
    )
endif()

function(atb_test target)
    target_include_directories(${target} PRIVATE src)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
    set_property(TARGET ${target} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endfunction()

if(BUILD_TESTING)
    set(ATB_TEST_PLUGIN_DIR "${CMAKE_BINARY_DIR}/test-runtime/plugin")
    add_custom_target(stage_bootstrap_test_plugin ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${ATB_TEST_PLUGIN_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:AmnesiaTheBunkerFpsFix>"
                "${ATB_TEST_PLUGIN_DIR}/AmnesiaTheBunker.FPSFix.asi"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.ini"
                "${ATB_TEST_PLUGIN_DIR}/AmnesiaTheBunker.FPSFix.ini"
        DEPENDS AmnesiaTheBunkerFpsFix
        COMMENT "Staging isolated telemetry-off plugin test files"
        VERBATIM
    )

    add_executable(frame_cadence_tests
        tests/frame_cadence_tests.cpp
        src/frame_cadence.cpp
    )
    atb_test(frame_cadence_tests)
    add_test(NAME frame_cadence COMMAND frame_cadence_tests)

    add_executable(config_tests
        tests/config_tests.cpp
        src/config.cpp
    )
    atb_test(config_tests)
    add_test(
        NAME hands_interpolation_profile
        COMMAND config_tests
                "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.hands-interpolation.ini"
    )
    add_test(
        NAME hands_pose_restore_profile
        COMMAND config_tests
                "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.hands-pose-restore-probe.ini"
                --pose-restore
    )
    add_test(
        NAME hands_pose_render_copy_profile
        COMMAND config_tests
                "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.hands-pose-render-copy-probe.ini"
                --pose-render-copy
    )
    add_test(
        NAME world_trace_profile
        COMMAND config_tests
                "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.world-trace.ini"
                --world-trace
    )
    add_test(
        NAME world_interpolation_profile
        COMMAND config_tests
                "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.world-interpolation.ini"
                --world-interpolation
    )
    add_test(
        NAME play_profile
        COMMAND config_tests
                "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.play.ini"
                --play
    )
    add_test(
        NAME hands_pose_interpolation_profile
        COMMAND config_tests
                "${CMAKE_SOURCE_DIR}/config/AmnesiaTheBunker.FPSFix.hands-pose-interpolation.ini"
                --pose-interpolation
    )

    add_executable(particle_interpolation_tests
        tests/particle_interpolation_tests.cpp
        src/particle_interpolation.cpp
    )
    atb_test(particle_interpolation_tests)
    add_test(NAME particle_interpolation COMMAND particle_interpolation_tests)

    add_executable(guarded_memory_tests
        tests/guarded_memory_tests.cpp
        src/guarded_memory.cpp
    )
    atb_test(guarded_memory_tests)
    add_test(NAME guarded_memory COMMAND guarded_memory_tests)

    add_executable(camera_trace_tests
        tests/camera_trace_tests.cpp
        src/camera_trace.cpp
        src/guarded_memory.cpp
    )
    atb_test(camera_trace_tests)
    add_test(NAME camera_trace COMMAND camera_trace_tests)

    add_executable(camera_interpolation_tests
        tests/camera_interpolation_tests.cpp
        src/camera_interpolation.cpp
    )
    atb_test(camera_interpolation_tests)
    add_test(NAME camera_interpolation COMMAND camera_interpolation_tests)

    add_executable(hands_trace_tests
        tests/hands_trace_tests.cpp
        src/guarded_memory.cpp
        src/hands_trace.cpp
    )
    atb_test(hands_trace_tests)
    add_test(NAME hands_trace COMMAND hands_trace_tests)

    add_executable(hands_pose_interpolation_tests
        tests/hands_pose_interpolation_tests.cpp
        src/hands_pose_interpolation.cpp
    )
    atb_test(hands_pose_interpolation_tests)
    add_test(NAME hands_pose_interpolation
             COMMAND hands_pose_interpolation_tests)

    add_executable(world_runtime_tests
        tests/world_runtime_tests.cpp
        src/hands_pose_interpolation.cpp
        src/hands_trace.cpp
        src/guarded_memory.cpp
        src/world_runtime.cpp
    )
    atb_test(world_runtime_tests)
    add_test(NAME world_runtime COMMAND world_runtime_tests)

    add_executable(camera_runtime_tests
        tests/camera_runtime_tests.cpp
        src/camera_interpolation.cpp
        src/camera_runtime.cpp
        src/guarded_memory.cpp
        src/hands_trace.cpp
    )
    atb_test(camera_runtime_tests)
    add_test(NAME camera_runtime COMMAND camera_runtime_tests)

    add_executable(presentation_clock_tests
        tests/presentation_clock_tests.cpp
        src/presentation_clock.cpp
    )
    atb_test(presentation_clock_tests)
    add_test(NAME presentation_clock COMMAND presentation_clock_tests)

    add_executable(presentation_limiter_tests
        tests/presentation_limiter_tests.cpp
        src/presentation_limiter.cpp
    )
    atb_test(presentation_limiter_tests)
    add_test(NAME presentation_limiter COMMAND presentation_limiter_tests)

    add_executable(frame_pacing_tests
        tests/frame_pacing_tests.cpp
        src/frame_pacing.cpp
    )
    atb_test(frame_pacing_tests)
    add_test(NAME frame_pacing COMMAND frame_pacing_tests)

    add_executable(release_config_tests
        tests/release_config_tests.cpp
        src/config.cpp
    )
    atb_test(release_config_tests)
    add_test(NAME release_config
             COMMAND release_config_tests
                     "${CMAKE_SOURCE_DIR}/packaging/AmnesiaTheBunker.FPSFix.ini")

    add_executable(gui_cursor_tests
        tests/gui_cursor_tests.cpp
    )
    atb_test(gui_cursor_tests)
    add_test(NAME gui_cursor COMMAND gui_cursor_tests)

    add_test(
        NAME split_validation_profile_transform
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/tools/run_split_validation.ps1"
                -DryRun
                -SettingsPath
                "${CMAKE_SOURCE_DIR}/tests/fixtures/split_validation_user_settings.cfg"
    )

    add_executable(target_mapping_tests
        tests/target_mapping_tests.cpp
        src/target_mapping.cpp
    )
    atb_test(target_mapping_tests)
    add_test(NAME target_mapping COMMAND target_mapping_tests)

    add_executable(runtime_mapping_tests
        tests/runtime_mapping_tests.cpp
        src/runtime_mapping.cpp
    )
    atb_test(runtime_mapping_tests)
    add_test(NAME runtime_mapping COMMAND runtime_mapping_tests)

    add_executable(telemetry_hook_tests
        src/camera_interpolation.cpp
        src/camera_runtime.cpp
        src/camera_trace.cpp
        tests/telemetry_hook_tests.cpp
        src/frame_cadence.cpp
        src/frame_pacing.cpp
        src/frame_trace.cpp
        src/guarded_memory.cpp
        src/gui_cursor.cpp
        src/hands_trace.cpp
        src/hands_pose_interpolation.cpp
        src/logger.cpp
        src/mouse_trace.cpp
        src/particle_runtime.cpp
        src/presentation_clock.cpp
        src/presentation_limiter.cpp
        src/telemetry.cpp
        src/world_runtime.cpp
    )
    atb_test(telemetry_hook_tests)
    target_link_libraries(telemetry_hook_tests PRIVATE minhook)
    add_test(NAME telemetry_hook COMMAND telemetry_hook_tests)
    add_test(NAME render_unlock_hook
             COMMAND telemetry_hook_tests --render-unlock-only)
    add_test(NAME presentation_limiter_hook
             COMMAND telemetry_hook_tests --presentation-limiter-only)
    add_test(NAME camera_trace_hook
             COMMAND telemetry_hook_tests --camera-trace-only)
    add_test(NAME camera_restore_probe_hook
             COMMAND telemetry_hook_tests --camera-restore-probe-only)
    add_test(NAME hands_trace_hook
             COMMAND telemetry_hook_tests --hands-trace-only)
    add_test(NAME hands_pose_restore_hook
             COMMAND telemetry_hook_tests --hands-pose-restore-only)

    add_executable(asi_bootstrap_host tests/asi_bootstrap_host.cpp)
    atb_test(asi_bootstrap_host)
    set_target_properties(asi_bootstrap_host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(
        NAME asi_bootstrap_loads_safely
        COMMAND asi_bootstrap_host
                "${ATB_TEST_PLUGIN_DIR}/AmnesiaTheBunker.FPSFix.asi"
    )

    if(ATB_BUILD_DEV_LAUNCHER)
        add_executable(injection_test_host tests/injection_test_host.cpp)
        atb_test(injection_test_host)
        set_target_properties(injection_test_host PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        add_test(
            NAME dev_launcher_injects_x64_asi
            COMMAND atb_dev_launcher
                    "$<TARGET_FILE:injection_test_host>"
                    "${ATB_TEST_PLUGIN_DIR}/AmnesiaTheBunker.FPSFix.asi"
                    --wait
        )
    endif()

    if(ATB_GAME_EXE)
        if(NOT EXISTS "${ATB_GAME_EXE}")
            message(FATAL_ERROR "ATB_GAME_EXE does not exist: ${ATB_GAME_EXE}")
        endif()
        add_executable(installed_target_tests
            tests/installed_target_tests.cpp
            src/runtime_mapping.cpp
            src/sha256.cpp
            src/target_mapping.cpp
        )
        atb_test(installed_target_tests)
        target_link_libraries(installed_target_tests PRIVATE bcrypt)
        add_test(
            NAME installed_target_identity_and_mapping
            COMMAND installed_target_tests "${ATB_GAME_EXE}"
        )
    endif()
endif()
