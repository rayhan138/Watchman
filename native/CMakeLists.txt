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
    update.cpp
    legacy_migration.cpp
    app_version.cpp
    update_policy.cpp
    update_checker.cpp
    update_worker.cpp
    main.cpp
    renderer.cpp
    metrics.cpp
    gpu_monitor.cpp
    temperature_monitor.cpp
    temperature_provider.cpp
    app_tracker.cpp
    ping_tester.cpp
    diagnostics.cpp
    storage.cpp
    traffic_export.cpp
    taskbar_embed.cpp
    widget_compositor.cpp
    bundled_fonts.cpp
    sqlite3.c
    speed_tester.cpp
    engine.cpp
    startup_diagnostics.cpp
    startup_registration.cpp
    startup_registration_worker.cpp
    tray_registration_worker.cpp
    watchman_resources.rc
    watchman.manifest
)

set(HEADERS
    renderer.h
    app_version.h
    update_policy.h
    update_checker.h
    update_preferences.h
    update_notification.h
    update_presentation.h
    settings_header_layout.h
    floating_monitor_settings_layout.h
    floating_monitor_layout.h
    floating_monitor_text.h
    widget_runtime.h
    widget_recovery_policy.h
    live_readings.h
    speed_test_state.h
    speed_test_view.h
    startup_diagnostics.h
    startup_registration.h
    startup_registration_policy.h
    tray_registration.h
    main_graphics_policy.h
    gpu_monitor.h
    history_files.h
    history_profile.h
    ui_responsiveness.h
    traffic_export.h
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
    version.h
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

# Shared Windows SDK dependencies for the shipped app and owned-window recovery test.
set(WATCHMAN_PLATFORM_LIBRARIES
    version.lib
    crypt32.lib
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
    oleaut32.lib
    taskschd.lib
    windowscodecs.lib
    gdi32.lib
)
target_link_libraries(Watchman PRIVATE ${WATCHMAN_PLATFORM_LIBRARIES})

# Set runtime settings
set_target_properties(Watchman PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# The release executable embeds its icon, logo, and Watchman font families.
# No separate font installer or runtime package is required.
install(TARGETS Watchman RUNTIME DESTINATION .)
install(FILES PORTABLE_README.txt DESTINATION .)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.h" WATCHMAN_VERSION_DEFINITION
    REGEX "^#define WATCHMAN_VERSION_TEXT \"[0-9]+\\.[0-9]+\\.[0-9]+\"$")
if(NOT WATCHMAN_VERSION_DEFINITION)
    message(FATAL_ERROR "Could not read WATCHMAN_VERSION_TEXT from version.h")
endif()
string(REGEX REPLACE
    "^#define WATCHMAN_VERSION_TEXT \"([0-9]+\\.[0-9]+\\.[0-9]+)\"$"
    "\\1" WATCHMAN_APP_VERSION "${WATCHMAN_VERSION_DEFINITION}")
set(CPACK_PACKAGE_NAME "Watchman")
set(CPACK_PACKAGE_VERSION "${WATCHMAN_APP_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "Watchman-${WATCHMAN_APP_VERSION}-win64-portable")
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
        engine.cpp startup_diagnostics.cpp metrics.cpp gpu_monitor.cpp
        temperature_monitor.cpp temperature_provider.cpp app_tracker.cpp
        ping_tester.cpp diagnostics.cpp storage.cpp traffic_export.cpp sqlite3.c speed_tester.cpp)
    target_include_directories(WatchmanRuntimeSmoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(WatchmanRuntimeSmoke PRIVATE
        iphlpapi ws2_32 windowsapp shlwapi wlanapi winhttp user32 advapi32
        dxgi pdh powrprof shell32 ole32 gdi32)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(TrayRegistrationTest tests/tray_registration_test.cpp tray_registration_worker.cpp)
    target_include_directories(TrayRegistrationTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(TrayRegistrationTest PRIVATE Threads::Threads)
    add_test(NAME TrayRegistration COMMAND TrayRegistrationTest)
    set_tests_properties(TrayRegistration PROPERTIES TIMEOUT 15)

    add_executable(UpdatePolicyTest tests/update_policy_test.cpp update_policy.cpp)
    target_include_directories(UpdatePolicyTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME UpdatePolicy COMMAND UpdatePolicyTest)

    add_executable(UpdateWorkerTest tests/update_worker_test.cpp update_worker.cpp)
    target_include_directories(UpdateWorkerTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(UpdateWorkerTest PRIVATE Threads::Threads)
    add_test(NAME UpdateWorker COMMAND UpdateWorkerTest)
    set_tests_properties(UpdateWorker PROPERTIES TIMEOUT 15)

    add_executable(StartupRegistrationPolicyTest tests/startup_registration_policy_test.cpp startup_registration_worker.cpp)
    target_include_directories(StartupRegistrationPolicyTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(StartupRegistrationPolicyTest PRIVATE Threads::Threads)
    add_test(NAME StartupRegistrationPolicy COMMAND StartupRegistrationPolicyTest)
    set_tests_properties(StartupRegistrationPolicy PROPERTIES TIMEOUT 15)

    if(WIN32)
        add_executable(UpdateWindowsTest tests/update_windows_test.cpp update_policy.cpp bundled_fonts.cpp watchman_resources.rc)
        target_include_directories(UpdateWindowsTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(UpdateWindowsTest PRIVATE dwrite gdi32 advapi32 shell32)
        add_test(NAME UpdateWindows COMMAND UpdateWindowsTest)

        add_executable(UpdateCheckerTest tests/update_checker_test.cpp update_checker.cpp update_policy.cpp app_version.cpp)
        target_include_directories(UpdateCheckerTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(UpdateCheckerTest PRIVATE WATCHMAN_UPDATE_CHECKING_TESTING=1)
        target_link_libraries(UpdateCheckerTest PRIVATE winhttp ws2_32 version)
        add_test(NAME UpdateChecker COMMAND UpdateCheckerTest)
        set_tests_properties(UpdateChecker PROPERTIES TIMEOUT 30)

        add_executable(StartupRegistrationTest tests/startup_registration_test.cpp startup_registration.cpp)
        target_include_directories(StartupRegistrationTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(StartupRegistrationTest PRIVATE WATCHMAN_STARTUP_REGISTRATION_TESTING=1)
        target_link_libraries(StartupRegistrationTest PRIVATE taskschd ole32 oleaut32 advapi32 shell32)
        add_test(NAME StartupRegistration COMMAND StartupRegistrationTest)
        set_tests_properties(StartupRegistration PROPERTIES TIMEOUT 45)
    endif()

    add_executable(WidgetRuntimeTest tests/widget_runtime_test.cpp)
    target_include_directories(WidgetRuntimeTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME WidgetRuntime COMMAND WidgetRuntimeTest)

    add_executable(TrafficExportTest tests/traffic_export_test.cpp traffic_export.cpp)
    target_include_directories(TrafficExportTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME TrafficExport COMMAND TrafficExportTest)

    add_executable(FloatingMonitorLayoutTest tests/floating_monitor_layout_test.cpp)
    target_include_directories(FloatingMonitorLayoutTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME FloatingMonitorLayout COMMAND FloatingMonitorLayoutTest)

    add_executable(HistoryProfileTest tests/history_profile_test.cpp)
    target_include_directories(HistoryProfileTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME HistoryProfile COMMAND HistoryProfileTest)

    add_executable(UiResponsivenessTest tests/ui_responsiveness_test.cpp)
    target_include_directories(UiResponsivenessTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME UiResponsiveness COMMAND UiResponsivenessTest)

    add_executable(ResponsivenessPolicyTest tests/responsiveness_policy_test.cpp)
    target_include_directories(ResponsivenessPolicyTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ResponsivenessPolicy COMMAND ResponsivenessPolicyTest)

    if(WIN32)
        add_executable(GpuMonitorTest tests/gpu_monitor_test.cpp gpu_monitor.cpp startup_diagnostics.cpp)
        target_include_directories(GpuMonitorTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(GpuMonitorTest PRIVATE dxgi pdh ole32)
        add_test(NAME GpuMonitor COMMAND GpuMonitorTest)
        set_tests_properties(GpuMonitor PROPERTIES TIMEOUT 15)

        add_executable(SpeedTesterLifecycleTest tests/speed_tester_lifecycle_test.cpp speed_tester.cpp)
        target_include_directories(SpeedTesterLifecycleTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(SpeedTesterLifecycleTest PRIVATE WATCHMAN_SPEED_TESTING=1)
        target_link_libraries(SpeedTesterLifecycleTest PRIVATE winhttp ws2_32 crypt32)
        add_test(NAME SpeedTesterLifecycle COMMAND SpeedTesterLifecycleTest)
        set_tests_properties(SpeedTesterLifecycle PROPERTIES TIMEOUT 45)

        # Compile-time faults are confined to windows owned by this test process.
        add_executable(StartupRecoveryTest tests/startup_recovery_test.cpp ${SOURCES})
        target_include_directories(StartupRecoveryTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(StartupRecoveryTest PRIVATE
            WATCHMAN_TASKBAR_TESTING=1 WATCHMAN_RENDER_TESTING=1 WATCHMAN_STARTUP_RECOVERY_TESTING=1)
        target_link_libraries(StartupRecoveryTest PRIVATE ${WATCHMAN_PLATFORM_LIBRARIES})
        add_test(NAME StartupRecovery COMMAND StartupRecoveryTest)
        set_tests_properties(StartupRecovery PROPERTIES TIMEOUT 45)
    endif()

    add_executable(SettingsHeaderLayoutTest tests/settings_header_layout_test.cpp)
    target_include_directories(SettingsHeaderLayoutTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME SettingsHeaderLayout COMMAND SettingsHeaderLayoutTest)

    add_executable(AppVersionTest tests/app_version_test.cpp app_version.cpp)
    target_include_directories(AppVersionTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_sources(AppVersionTest PRIVATE tests/app_version_resource_test.rc)
        target_compile_definitions(AppVersionTest PRIVATE WATCHMAN_VERSION_RESOURCE_TEST=1)
        target_link_libraries(AppVersionTest PRIVATE version)

        add_executable(AppVersionUnversionedTest tests/app_version_test.cpp app_version.cpp)
        target_include_directories(AppVersionUnversionedTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(AppVersionUnversionedTest PRIVATE version)
        add_test(NAME AppVersionUnversioned COMMAND AppVersionUnversionedTest)
    endif()
    add_test(NAME AppVersion COMMAND AppVersionTest)

    add_executable(WatchmanUpdateProtocolProbe update.cpp)
    target_compile_definitions(WatchmanUpdateProtocolProbe PRIVATE
        WATCHMAN_PROTOCOL_PROBE WATCHMAN_PROTOCOL_PROBE_MAJOR=1
        WATCHMAN_PROTOCOL_PROBE_MINOR=0 WATCHMAN_PROTOCOL_PROBE_PATCH=5)
    target_link_libraries(WatchmanUpdateProtocolProbe PRIVATE crypt32 version shell32 ole32 winhttp)

    add_executable(WatchmanUpdateProtocolProbeV2 update.cpp)
    target_compile_definitions(WatchmanUpdateProtocolProbeV2 PRIVATE
        WATCHMAN_PROTOCOL_PROBE WATCHMAN_PROTOCOL_PROBE_MAJOR=2
        WATCHMAN_PROTOCOL_PROBE_MINOR=0 WATCHMAN_PROTOCOL_PROBE_PATCH=0)
    target_link_libraries(WatchmanUpdateProtocolProbeV2 PRIVATE crypt32 version shell32 ole32 winhttp)

    add_executable(WatchmanUpdateProtocolProbeV201 update.cpp)
    target_compile_definitions(WatchmanUpdateProtocolProbeV201 PRIVATE
        WATCHMAN_PROTOCOL_PROBE WATCHMAN_PROTOCOL_PROBE_MAJOR=2
        WATCHMAN_PROTOCOL_PROBE_MINOR=0 WATCHMAN_PROTOCOL_PROBE_PATCH=1)
    target_link_libraries(WatchmanUpdateProtocolProbeV201 PRIVATE crypt32 version shell32 ole32 winhttp)
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

    add_executable(HistoryStorageTest tests/history_storage_test.cpp storage.cpp traffic_export.cpp sqlite3.c)
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
