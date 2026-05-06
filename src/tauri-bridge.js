/**
 * Tauri Bridge — Compatibility shim that maps the old Electron `window.systemAPI`
 * interface to Tauri's `invoke()` and `listen()` APIs.
 * 
 * This means renderer.js needs ZERO changes — it continues using window.systemAPI.
 */

(function() {
  'use strict';

  const { invoke } = window.__TAURI__.core;
  const { listen } = window.__TAURI__.event;
  const { getCurrentWindow } = window.__TAURI__.window;

  // Build the systemAPI object that mirrors the Electron preload bridge
  window.systemAPI = {
    // ====== System Monitoring ======
    getCpuUsage: () => invoke('cmd_get_cpu_usage'),
    getMemoryUsage: () => invoke('cmd_get_memory_usage'),
    getNetworkStats: () => invoke('cmd_get_network_stats'),
    resetSessionCounters: () => invoke('cmd_reset_session_counters'),
    getDiskUsage: () => invoke('cmd_get_disk_usage'),
    getTemperatureReadings: () => invoke('cmd_get_temperature_readings'),
    getNetworkInterfaces: () => invoke('cmd_get_network_interfaces'),
    getSystemInfo: () => invoke('cmd_get_system_info'),

    // ====== Config ======
    getConfig: () => invoke('get_config'),
    saveConfig: (cfg) => invoke('save_config', { newConfig: cfg }),
    applyRecommendedSettings: () => invoke('apply_recommended_settings'),
    undoSettings: () => invoke('undo_settings'),
    canUndoSettings: () => invoke('can_undo_settings'),

    // ====== Traffic History ======
    getTrafficHistory: (viewType) => invoke('get_traffic_history', { viewType }),

    // ====== Network Health ======
    getQuality: (downloadSpeed, latency) => invoke('get_quality', { downloadSpeed, latency }),
    measureLatency: () => invoke('measure_latency'),
    getSignalStrength: () => invoke('get_signal_strength'),
    getNetworkOverview: () => invoke('get_network_overview'),
    runSpeedTest: () => invoke('run_speed_test'),
    getSpeedTestHistory: () => invoke('get_speed_test_history'),

    // ====== Application Monitor ======
    getActiveApplications: () => invoke('get_active_applications'),
    getAppMonitorStatus: () => invoke('get_app_monitor_status'),
    terminateApplication: (pid) => invoke('terminate_application', { pid }),

    // ====== Data Usage ======
    getUsage: (period) => invoke('get_usage', { period }),
    setDataLimit: (limitBytes) => invoke('set_data_limit', { limitBytes }),
    getRemainingAllowance: () => invoke('get_remaining_allowance'),
    getDataThresholds: () => invoke('get_data_thresholds'),
    compareUsage: (period) => invoke('compare_usage', { period }),

    // ====== Profiles ======
    getProfiles: () => invoke('get_profiles'),
    getActiveProfile: () => invoke('get_active_profile'),
    setActiveProfile: (profileId) => invoke('set_active_profile', { profileId }),
    getProfileConfig: (profileId) => invoke('get_profile_config', { profileId }),

    // ====== Troubleshooter ======
    runDiagnostics: () => invoke('run_diagnostics'),

    // ====== Export ======
    exportCSV: (options) => invoke('export_csv', { options }),

    // ====== Notifications ======
    dismissNotification: (id) => invoke('dismiss_notification', { notificationId: id }),
    handleNotificationAction: (id, action) => invoke('notification_action', { notificationId: id, action }),

    // ====== Window Management ======
    minimizeWindow: () => invoke('cmd_minimize_window'),
    closeWindow: () => invoke('cmd_close_window'),
    toggleAlwaysOnTop: () => invoke('cmd_toggle_always_on_top'),

    // ====== Event Listeners ======
    onMetrics: (callback) => {
      listen('metrics', (event) => {
        callback(event.payload);
      });
    },

    onETWNetworkStats: (callback) => {
      // ETW is not needed in Tauri — network stats come from sysinfo
      // This is a no-op to prevent errors
    },

    onNotification: (callback) => {
      listen('notification', (event) => {
        callback(event.payload);
      });
    },
  };

  // Window dragging support for frameless window
  document.addEventListener('DOMContentLoaded', () => {
    const titleBar = document.querySelector('.title-bar');
    if (titleBar) {
      titleBar.addEventListener('mousedown', (e) => {
        if (e.target.closest('button') || e.target.closest('.tab-bar')) return;
        getCurrentWindow().startDragging();
      });
    }
  });

  console.log('[Tauri Bridge] window.systemAPI initialized');
})();
