cmake_minimum_required(VERSION 3.20)
project(Watchman LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure compiler flags for MSVC
if(MSVC)
    add_compile_options(/W4 /WX- /EHsc /std:c++17 /permissive-)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)
    # Ship the C++ runtime inside Watchman so a tester does not need to install
    # the Visual C++ Redistributable separately.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Source files
set(SOURCES
    main.cpp
    renderer.cpp
    metrics.cpp
    temperature_monitor.cpp
    temperature_provider.cpp
    app_tracker.cpp
    ping_tester.cpp
    diagnostics.cpp
    storage.cpp
    taskbar_embed.cpp
    widget_compositor.cpp
    bundled_fonts.cpp
    sqlite3.c
    speed_tester.cpp
    engine.cpp
    watchman_resources.rc
)

set(HEADERS
    renderer.h
    floating_monitor_settings_layout.h
    floating_monitor_layout.h
    taskbar_widget_layout.h
    taskbar_widget_mode.h
    widget_compositor.h
    bundled_fonts.h
    metrics.h
    temperature_monitor.h
    temperature_provider.h
    temperature_sensors.h
    sqlite3.h
    app_tracker.h
    ping_tester.h
    diagnostics.h
    storage.h
    ui_style.h
    speed_tester.h
    engine.h
    snapshots.h
    resource.h
    telemetry_math.h
    network_test_protocol.h
    light_theme_reference.h
    main_content_layout.h
    app_traffic_presentation.h
    runtime_policy.h
    window_lifecycle.h
    warning_policy.h
    warnings_settings_layout.h
    settings_text_layout.h
    theme_picker_layout.h
    welcome_tour.h
    welcome_tour_preferences.h
    speed_test_presentation.h
    scoped_handle.h
)

# Add executable target (GUI app, no console window in Release)
add_executable(Watchman WIN32 ${SOURCES} ${HEADERS})

# Link necessary Windows SDK libraries
target_link_libraries(Watchman PRIVATE
    d2d1.lib
    dwrite.lib
    dwmapi.lib
    iphlpapi.lib
    ws2_32.lib
    windowsapp.lib
    comctl32.lib
    comdlg32.lib
    shlwapi.lib
    wlanapi.lib
    winhttp.lib
    user32.lib
    advapi32.lib
    dxgi.lib
    d3d11.lib
    dcomp.lib
    pdh.lib
    powrprof.lib
    shell32.lib
    ole32.lib
    windowscodecs.lib
    gdi32.lib
)

# Set runtime settings
set_target_properties(Watchman PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# The release executable embeds its icon, logo, and Watchman font families.
# No separate font installer or runtime package is required.
install(TARGETS Watchman RUNTIME DESTINATION .)
install(FILES PORTABLE_README.txt DESTINATION .)
set(CPACK_PACKAGE_NAME "Watchman")
set(CPACK_PACKAGE_VERSION "2.0.1")
set(CPACK_PACKAGE_FILE_NAME "Watchman-2.0.1-win64-portable")
set(CPACK_GENERATOR "ZIP")
include(CPack)

# Add Gradient Demo target
add_executable(GradientDemo WIN32 ui_gradient_demo/Direct2D_Gradient_Demo.cpp)
target_link_libraries(GradientDemo PRIVATE d2d1.lib dwrite.lib dwmapi.lib user32.lib shell32.lib)

# Opt-in integration tooling: this launches real services and needs disposable
# Windows runner data. It must never become a synthetic/offline CTest.
option(WATCHMAN_BUILD_RUNTIME_SMOKE "Build the disposable Windows runtime-smoke probe" OFF)
if(WIN32 AND WATCHMAN_BUILD_RUNTIME_SMOKE)
    add_executable(WatchmanRuntimeSmoke
        tests/runtime_smoke_probe.cpp
        engine.cpp metrics.cpp temperature_monitor.cpp temperature_provider.cpp
        app_tracker.cpp ping_tester.cpp diagnostics.cpp storage.cpp sqlite3.c speed_tester.cpp)
    target_include_directories(WatchmanRuntimeSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(WatchmanRuntimeSmoke PRIVATE
        iphlpapi ws2_32 windowsapp shlwapi wlanapi winhttp user32 advapi32
        dxgi pdh powrprof shell32 ole32 gdi32)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(FloatingMonitorSettingsLayoutTest tests/floating_monitor_settings_layout_test.cpp)
    target_include_directories(FloatingMonitorSettingsLayoutTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(FloatingMonitorSettingsLayoutTest PRIVATE gdi32)
    add_test(NAME FloatingMonitorSettingsLayout COMMAND FloatingMonitorSettingsLayoutTest)

    add_executable(ThemePaletteTest tests/theme_palette_test.cpp)
    target_include_directories(ThemePaletteTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ThemePalette COMMAND ThemePaletteTest)

    add_executable(SettingsLayoutTest tests/settings_layout_test.cpp)
    target_include_directories(SettingsLayoutTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME SettingsLayout COMMAND SettingsLayoutTest)

    add_executable(WelcomeTourTest tests/welcome_tour_test.cpp)
    target_include_directories(WelcomeTourTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME WelcomeTour COMMAND WelcomeTourTest)

    if(WIN32)
        add_executable(WelcomeTourPreferencesTest tests/welcome_tour_preferences_test.cpp)
        target_include_directories(WelcomeTourPreferencesTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(WelcomeTourPreferencesTest PRIVATE advapi32)
        add_test(NAME WelcomeTourPreferences COMMAND WelcomeTourPreferencesTest)

        add_executable(BundledFontsTest tests/bundled_fonts_test.cpp bundled_fonts.cpp watchman_resources.rc)
        target_include_directories(BundledFontsTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(BundledFontsTest PRIVATE dwrite gdi32)
        add_test(NAME BundledFonts COMMAND BundledFontsTest)

        add_executable(SettingsTextLayoutTest tests/settings_text_layout_test.cpp bundled_fonts.cpp watchman_resources.rc)
        target_include_directories(SettingsTextLayoutTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(SettingsTextLayoutTest PRIVATE dwrite gdi32)
        add_test(NAME SettingsTextLayout COMMAND SettingsTextLayoutTest)
    endif()

    add_executable(TaskbarWidgetLayoutTest tests/taskbar_widget_layout_test.cpp)
    target_include_directories(TaskbarWidgetLayoutTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_sources(TaskbarWidgetLayoutTest PRIVATE bundled_fonts.cpp watchman_resources.rc)
        target_link_libraries(TaskbarWidgetLayoutTest PRIVATE dwrite gdi32)
    endif()
    add_test(NAME TaskbarWidgetLayoutTest COMMAND TaskbarWidgetLayoutTest)

    add_executable(TelemetryMathTest tests/telemetry_math_test.cpp)
    target_include_directories(TelemetryMathTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME TelemetryMath COMMAND TelemetryMathTest)

    add_executable(TemperatureSensorsTest tests/temperature_sensors_test.cpp)
    target_include_directories(TemperatureSensorsTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME TemperatureSensors COMMAND TemperatureSensorsTest)

    add_executable(TemperatureProviderTest tests/temperature_provider_test.cpp temperature_provider.cpp)
    target_include_directories(TemperatureProviderTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(TemperatureProviderTest PRIVATE windowsapp)
    add_test(NAME TemperatureProvider COMMAND TemperatureProviderTest)

    add_executable(HistoryStorageTest tests/history_storage_test.cpp storage.cpp sqlite3.c)
    target_include_directories(HistoryStorageTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(HistoryStorageTest PRIVATE shell32 ole32)
    add_test(NAME HistoryStorage COMMAND HistoryStorageTest)

    add_executable(NetworkTestProtocolTest tests/network_test_protocol_test.cpp)
    target_include_directories(NetworkTestProtocolTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME NetworkTestProtocol COMMAND NetworkTestProtocolTest)

    add_executable(LightThemeReferenceTest tests/light_theme_reference_test.cpp)
    target_include_directories(LightThemeReferenceTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME LightThemeReference COMMAND LightThemeReferenceTest)

    add_executable(MainReadabilityLayoutTest tests/main_readability_layout_test.cpp)
    target_include_directories(MainReadabilityLayoutTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(MainReadabilityLayoutTest PRIVATE gdi32)
    add_test(NAME MainReadabilityLayout COMMAND MainReadabilityLayoutTest "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(AppTrafficPresentationTest tests/app_traffic_presentation_test.cpp)
    target_include_directories(AppTrafficPresentationTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME AppTrafficPresentation COMMAND AppTrafficPresentationTest)

    if(WIN32)
        add_executable(AppTrackerCountersTest tests/app_tracker_counters_test.cpp)
        target_include_directories(AppTrackerCountersTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME AppTrackerCounters COMMAND AppTrackerCountersTest)
    endif()

    add_executable(RuntimePolicyTest tests/runtime_policy_test.cpp)
    target_include_directories(RuntimePolicyTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(RuntimePolicyTest PRIVATE shell32)
    add_test(NAME RuntimePolicy COMMAND RuntimePolicyTest)

    add_executable(WarningPolicyTest tests/warning_policy_test.cpp)
    target_include_directories(WarningPolicyTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME WarningPolicy COMMAND WarningPolicyTest)

    add_executable(SpeedTestPresentationTest tests/speed_test_presentation_test.cpp)
    target_include_directories(SpeedTestPresentationTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME SpeedTestPresentation COMMAND SpeedTestPresentationTest)

    add_executable(WindowLifecycleTest tests/window_lifecycle_test.cpp)
    target_include_directories(WindowLifecycleTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(WindowLifecycleTest PRIVATE user32)
    add_test(NAME WindowLifecycle COMMAND WindowLifecycleTest)

    add_executable(ScopedHandleTest tests/scoped_handle_test.cpp)
    target_include_directories(ScopedHandleTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(ScopedHandleTest PRIVATE advapi32)
    endif()
    add_test(NAME ScopedHandle COMMAND ScopedHandleTest)
endif()
