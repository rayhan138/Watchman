// renderer.js — Frontend logic for Electron
(function () {
  'use strict';

  // ====== Constants ======
  const CHART_POINTS = 60;   // 60 seconds of history

  // ====== State ======
  const state = {
    downloadHistory: new Array(CHART_POINTS).fill(0),
    uploadHistory: new Array(CHART_POINTS).fill(0),
    trafficChart: null,
    dataUsageChart: null,
    dashboardHistoryChart: null,
    dataUsageHistoryChart: null,
    isConnected: false,
    systemInfoLoaded: false,
    networkDetailsLoaded: false,
    pinned: false,
    currentTab: 'dashboard',
    tabsInitialized: new Set(),
    appPollInterval: null,
    healthPollInterval: null,
    latencyPollInterval: null,
    currentDataPeriod: 'daily',
    exportFormat: 'csv',

    peakSpeed: 10, // MB/s, for traffic light calculation
    currentDownloadSpeed: 0, // bytes/sec, live from metrics
    dashCurrentPeriod: 'daily', // Dashboard usage summary period
    exportAvailability: {
      loaded: false,
      hasHistory: false,
      months: [],
      years: [],
      monthSet: new Set(),
      yearSet: new Set()
    }
  };

  // ====== DOM References ======
  const dom = {
    // Speed
    downloadSpeed: document.getElementById('downloadSpeed'),
    downloadUnit: document.getElementById('downloadUnit'),
    uploadSpeed: document.getElementById('uploadSpeed'),
    uploadUnit: document.getElementById('uploadUnit'),
    totalDownloaded: document.getElementById('totalDownloaded'),
    totalUploaded: document.getElementById('totalUploaded'),

    // Gauges
    cpuValue: document.getElementById('cpuValue'),
    cpuRing: document.getElementById('cpuRing'),
    cpuCores: document.getElementById('cpuCores'),
    ramValue: document.getElementById('ramValue'),
    ramRing: document.getElementById('ramRing'),
    ramInfo: document.getElementById('ramInfo'),
    diskValue: document.getElementById('diskValue'),
    diskRing: document.getElementById('diskRing'),
    diskInfo: document.getElementById('diskInfo'),

    // History
    historyViewType: document.getElementById('historyViewType'),
    historyTableBody: document.getElementById('historyTableBody'),

    // Details
    networkDetailsTable: document.getElementById('networkDetailsTable'),
    systemInfoTable: document.getElementById('systemInfoTable'),

    // Status
    statusDot: document.getElementById('statusDot'),
    connectionStatus: document.getElementById('connectionStatus'),
    uptimeDisplay: document.getElementById('uptimeDisplay'),

    // Controls
    minimizeBtn: document.getElementById('minimizeBtn'),
    closeBtn: document.getElementById('closeBtn'),
    pinBtn: document.getElementById('pinBtn'),
    networkDetailsToggle: document.getElementById('networkDetailsToggle'),
    networkDetailsPanel: document.getElementById('networkDetailsPanel'),
    systemInfoToggle: document.getElementById('systemInfoToggle'),
    systemInfoPanel: document.getElementById('systemInfoPanel'),
    themeToggleBtn: document.getElementById('themeToggleBtn'),
    themeIcon: document.getElementById('themeIcon'),
    
    // Settings
    settingsBtn: document.getElementById('settingsBtn'),
    settingsModal: document.getElementById('settingsModal'),
    settingsCloseBtn: document.getElementById('settingsCloseBtn'),
    cfgStartOnBoot: document.getElementById('cfgStartOnBoot'),
    cfgUnitMode: document.getElementById('cfgUnitMode'),
    cfgHideGauges: document.getElementById('cfgHideGauges'),
    gaugesSection: document.querySelector('.gauges-section'),

    // Traffic Graph (Task 16)
    trafficChart: document.getElementById('trafficChart'),
    trafficLight: document.getElementById('trafficLight'),
    tlDot: document.getElementById('tlDot'),
    tlLabel: document.getElementById('tlLabel'),
    tlDetail: document.getElementById('tlDetail'),

    // Tabs
    tabBar: document.getElementById('tabBar'),

    // Data Usage (Task 12)
    duTotalUsage: document.getElementById('duTotalUsage'),
    duDownload: document.getElementById('duDownload'),
    duUpload: document.getElementById('duUpload'),
    duComparisonValue: document.getElementById('duComparisonValue'),
    duComparisonTrend: document.getElementById('duComparisonTrend'),
    duLimitRemaining: document.getElementById('duLimitRemaining'),
    duLimitProgress: document.getElementById('duLimitProgress'),
    duLimitFill: document.getElementById('duLimitFill'),
    duLimitPct: document.getElementById('duLimitPct'),
    duLimitInput: document.getElementById('duLimitInput'),
    duLimitSaveBtn: document.getElementById('duLimitSaveBtn'),
    dataUsageChartCanvas: document.getElementById('dataUsageChart'),

    // Dashboard Usage Summary
    dashUsagePeriodChip: document.getElementById('dashUsagePeriodChip'),
    dashUsageMeta: document.getElementById('dashUsageMeta'),
    dashTotalUsage: document.getElementById('dashTotalUsage'),
    dashDownload: document.getElementById('dashDownload'),
    dashUpload: document.getElementById('dashUpload'),

    // Applications (Task 14)
    appHeaderSubtitle: document.getElementById('appHeaderSubtitle'),
    appCountBadge: document.getElementById('appCountBadge'),
    appTableBody: document.getElementById('appTableBody'),

    // Network Health (Task 13)
    nhQualityCard: document.getElementById('nhQualityCard'),
    nhQualityDot: document.getElementById('nhQualityDot'),
    nhQualityLevel: document.getElementById('nhQualityLevel'),
    nhQualitySub: document.getElementById('nhQualitySub'),
    nhLatencyValue: document.getElementById('nhLatencyValue'),
    nhLatencyLabel: document.getElementById('nhLatencyLabel'),
    nhJitterValue: document.getElementById('nhJitterValue'),
    nhJitterLabel: document.getElementById('nhJitterLabel'),
    nhLossValue: document.getElementById('nhLossValue'),
    nhLossLabel: document.getElementById('nhLossLabel'),
    nhConnType: document.getElementById('nhConnType'),
    nhConnName: document.getElementById('nhConnName'),
    nhConnLink: document.getElementById('nhConnLink'),
    nhConnIp: document.getElementById('nhConnIp'),
    nhSignalBars: document.getElementById('nhSignalBars'),
    nhSignalLabel: document.getElementById('nhSignalLabel'),
    nhSpeedTestBtn: document.getElementById('nhSpeedTestBtn'),
    nhSpeedTestProgress: document.getElementById('nhSpeedTestProgress'),
    nhSpeedTestResults: document.getElementById('nhSpeedTestResults'),
    nhStDownload: document.getElementById('nhStDownload'),
    nhStUpload: document.getElementById('nhStUpload'),
    nhStPing: document.getElementById('nhStPing'),
    nhStServer: document.getElementById('nhStServer'),
    nhStTime: document.getElementById('nhStTime'),

    // Tools (Task 18 + 19)
    toolsTroubleshootBtn: document.getElementById('toolsTroubleshootBtn'),
    toolsTroubleshootProgress: document.getElementById('toolsTroubleshootProgress'),
    toolsTroubleshootResults: document.getElementById('toolsTroubleshootResults'),
    toolsFixes: document.getElementById('toolsFixes'),
    toolsFixesList: document.getElementById('toolsFixesList'),
    exportPeriodSelect: document.getElementById('exportPeriodSelect'),
    exportMonthRow: document.getElementById('exportMonthRow'),
    exportMonthSelect: document.getElementById('exportMonthSelect'),
    exportYearRow: document.getElementById('exportYearRow'),
    exportYearSelect: document.getElementById('exportYearSelect'),
    exportHelper: document.getElementById('exportHelper'),
    exportBtn: document.getElementById('exportBtn'),
    exportStatus: document.getElementById('exportStatus'),



    // Enhanced Settings (Task 20)
    cfgDataLimit: document.getElementById('cfgDataLimit'),
    cfgWarnTrafficEnabled: document.getElementById('cfgWarnTrafficEnabled'),
    cfgWarnTrafficThreshold: document.getElementById('cfgWarnTrafficThreshold'),
    cfgWarnTrafficUnit: document.getElementById('cfgWarnTrafficUnit'),
    cfgWarnMemoryEnabled: document.getElementById('cfgWarnMemoryEnabled'),
    cfgWarnMemoryThreshold: document.getElementById('cfgWarnMemoryThreshold'),
    cfgWarnCpuTempEnabled: document.getElementById('cfgWarnCpuTempEnabled'),
    cfgWarnCpuTempThreshold: document.getElementById('cfgWarnCpuTempThreshold'),
    cfgWarnGpuTempEnabled: document.getElementById('cfgWarnGpuTempEnabled'),
    cfgWarnGpuTempThreshold: document.getElementById('cfgWarnGpuTempThreshold'),
    cfgWarnDiskTempEnabled: document.getElementById('cfgWarnDiskTempEnabled'),
    cfgWarnDiskTempThreshold: document.getElementById('cfgWarnDiskTempThreshold'),
    cfgWarnMainboardTempEnabled: document.getElementById('cfgWarnMainboardTempEnabled'),
    cfgWarnMainboardTempThreshold: document.getElementById('cfgWarnMainboardTempThreshold'),
    cfgRecommendedBtn: document.getElementById('cfgRecommendedBtn'),
    cfgUndoBtn: document.getElementById('cfgUndoBtn'),
    cfgSaveBtn: document.getElementById('cfgSaveBtn'),

    // Confirm Dialog
    confirmModal: document.getElementById('confirmModal'),
    confirmTitle: document.getElementById('confirmTitle'),
    confirmMessage: document.getElementById('confirmMessage'),
    confirmCancelBtn: document.getElementById('confirmCancelBtn'),
    confirmOkBtn: document.getElementById('confirmOkBtn'),

    // Tooltip
    globalTooltip: document.getElementById('globalTooltip'),

    // Toast
    toastContainer: document.getElementById('toastContainer')
  };

  let localConfig = {};
  let settingsBaselineConfig = null;

  // ====== Utility Functions ======

  function cloneConfig(config) {
    return JSON.parse(JSON.stringify(config || {}));
  }

  function formatSpeed(bytesPerSec) {
    let val = bytesPerSec;
    let unitsBase = ['B/s', 'KB/s', 'MB/s', 'GB/s', 'TB/s'];
    let multiplier = 1024;
    
    if (localConfig.unitModeBits) {
      val = bytesPerSec * 8;
      unitsBase = ['bps', 'Kbps', 'Mbps', 'Gbps', 'Tbps'];
      multiplier = 1000;
    }

    if (val < Math.pow(multiplier, 2)) {
      // Force minimum to KB/s (or Kbps) by skipping the zero index
      return { value: (val / multiplier).toFixed(1), unit: unitsBase[1] };
    } else if (val < Math.pow(multiplier, 3)) {
      return { value: (val / Math.pow(multiplier, 2)).toFixed(2), unit: unitsBase[2] };
    } else {
      return { value: (val / Math.pow(multiplier, 3)).toFixed(2), unit: unitsBase[3] };
    }
  }

  function formatBytes(bytes) {
    if (bytes === 0) return '0 B';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(1024));
    return (bytes / Math.pow(1024, i)).toFixed(i > 1 ? 2 : 0) + ' ' + units[i];
  }

  function formatUptime(seconds) {
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const mins = Math.floor((seconds % 3600) / 60);
    if (days > 0) return `${days}d ${hours}h ${mins}m`;
    if (hours > 0) return `${hours}h ${mins}m`;
    return `${mins}m`;
  }

  function updateGauge(fillEl, valueEl, percent) {
    if (fillEl) fillEl.style.height = `${percent}%`;
    if (valueEl) valueEl.textContent = Math.round(percent);
  }

  function getDashPeriodChipLabel(period) {
    switch (period) {
      case 'weekly':
        return 'This Week';
      case 'monthly':
        return 'This Month';
      default:
        return 'Today';
    }
  }

  function getNextDashPeriod(period) {
    if (period === 'daily') return 'weekly';
    if (period === 'weekly') return 'monthly';
    return 'daily';
  }

  function getDefaultWarningSettings(sourceConfig = localConfig) {
    const notifications = sourceConfig.notifications || {};
    const warningSettings = notifications.warningSettings || {};

    return {
      trafficEnabled: warningSettings.trafficEnabled === true,
      trafficThreshold: warningSettings.trafficThreshold ?? 500,
      trafficUnit: warningSettings.trafficUnit || 'MB',
      memoryEnabled: warningSettings.memoryEnabled ?? (sourceConfig.memoryWarningEnabled !== false),
      memoryThreshold: warningSettings.memoryThreshold ?? sourceConfig.memoryWarningThreshold ?? 80,
      cpuTempEnabled: warningSettings.cpuTempEnabled !== false,
      cpuTempThreshold: warningSettings.cpuTempThreshold ?? 80,
      gpuTempEnabled: warningSettings.gpuTempEnabled !== false,
      gpuTempThreshold: warningSettings.gpuTempThreshold ?? 80,
      diskTempEnabled: warningSettings.diskTempEnabled !== false,
      diskTempThreshold: warningSettings.diskTempThreshold ?? 80,
      mainboardTempEnabled: warningSettings.mainboardTempEnabled !== false,
      mainboardTempThreshold: warningSettings.mainboardTempThreshold ?? 80
    };
  }

  function applyWarningSettingsToDOM(sourceConfig = localConfig) {
    const warningSettings = getDefaultWarningSettings(sourceConfig);

    if (dom.cfgWarnTrafficEnabled) dom.cfgWarnTrafficEnabled.checked = warningSettings.trafficEnabled;
    if (dom.cfgWarnTrafficThreshold) dom.cfgWarnTrafficThreshold.value = warningSettings.trafficThreshold;
    if (dom.cfgWarnTrafficUnit) dom.cfgWarnTrafficUnit.value = warningSettings.trafficUnit;
    if (dom.cfgWarnMemoryEnabled) dom.cfgWarnMemoryEnabled.checked = warningSettings.memoryEnabled;
    if (dom.cfgWarnMemoryThreshold) dom.cfgWarnMemoryThreshold.value = warningSettings.memoryThreshold;
    if (dom.cfgWarnCpuTempEnabled) dom.cfgWarnCpuTempEnabled.checked = warningSettings.cpuTempEnabled;
    if (dom.cfgWarnCpuTempThreshold) dom.cfgWarnCpuTempThreshold.value = warningSettings.cpuTempThreshold;
    if (dom.cfgWarnGpuTempEnabled) dom.cfgWarnGpuTempEnabled.checked = warningSettings.gpuTempEnabled;
    if (dom.cfgWarnGpuTempThreshold) dom.cfgWarnGpuTempThreshold.value = warningSettings.gpuTempThreshold;
    if (dom.cfgWarnDiskTempEnabled) dom.cfgWarnDiskTempEnabled.checked = warningSettings.diskTempEnabled;
    if (dom.cfgWarnDiskTempThreshold) dom.cfgWarnDiskTempThreshold.value = warningSettings.diskTempThreshold;
    if (dom.cfgWarnMainboardTempEnabled) dom.cfgWarnMainboardTempEnabled.checked = warningSettings.mainboardTempEnabled;
    if (dom.cfgWarnMainboardTempThreshold) dom.cfgWarnMainboardTempThreshold.value = warningSettings.mainboardTempThreshold;
  }

  function collectWarningSettingsFromDOM() {
    return {
      trafficEnabled: !!dom.cfgWarnTrafficEnabled?.checked,
      trafficThreshold: parseInt(dom.cfgWarnTrafficThreshold?.value || '500', 10) || 500,
      trafficUnit: dom.cfgWarnTrafficUnit?.value || 'MB',
      memoryEnabled: !!dom.cfgWarnMemoryEnabled?.checked,
      memoryThreshold: parseInt(dom.cfgWarnMemoryThreshold?.value || '80', 10) || 80,
      cpuTempEnabled: !!dom.cfgWarnCpuTempEnabled?.checked,
      cpuTempThreshold: parseInt(dom.cfgWarnCpuTempThreshold?.value || '80', 10) || 80,
      gpuTempEnabled: !!dom.cfgWarnGpuTempEnabled?.checked,
      gpuTempThreshold: parseInt(dom.cfgWarnGpuTempThreshold?.value || '80', 10) || 80,
      diskTempEnabled: !!dom.cfgWarnDiskTempEnabled?.checked,
      diskTempThreshold: parseInt(dom.cfgWarnDiskTempThreshold?.value || '80', 10) || 80,
      mainboardTempEnabled: !!dom.cfgWarnMainboardTempEnabled?.checked,
      mainboardTempThreshold: parseInt(dom.cfgWarnMainboardTempThreshold?.value || '80', 10) || 80
    };
  }

  function applySettingsModalToDOM(sourceConfig = localConfig) {
    dom.cfgStartOnBoot.checked = !!sourceConfig.startOnBoot;
    dom.cfgUnitMode.checked = !!sourceConfig.unitModeBits;
    dom.cfgHideGauges.checked = !!sourceConfig.hideGauges;
    dom.cfgDataLimit.value = sourceConfig.dataLimit
      ? (sourceConfig.dataLimit / (1024 * 1024 * 1024)).toFixed(0)
      : '';
    applyWarningSettingsToDOM(sourceConfig);
  }

  function collectSettingsConfigFromDOM(baseConfig = localConfig) {
    const nextConfig = cloneConfig(baseConfig);
    const warningSettings = collectWarningSettingsFromDOM();
    const anyWarningEnabled =
      warningSettings.trafficEnabled ||
      warningSettings.memoryEnabled ||
      warningSettings.cpuTempEnabled ||
      warningSettings.gpuTempEnabled ||
      warningSettings.diskTempEnabled ||
      warningSettings.mainboardTempEnabled;
    const limitGb = parseFloat(dom.cfgDataLimit?.value || '0') || 0;
    const limitBytes = Math.round(limitGb * 1024 * 1024 * 1024);

    nextConfig.startOnBoot = !!dom.cfgStartOnBoot.checked;
    nextConfig.unitModeBits = !!dom.cfgUnitMode.checked;
    nextConfig.hideGauges = !!dom.cfgHideGauges.checked;
    nextConfig.dataLimit = limitBytes;
    nextConfig.dataLimitEnabled = limitBytes > 0;
    nextConfig.notifications = {
      ...(nextConfig.notifications || {}),
      enabled: true,
      dataUsageAlerts: false,
      slowInternetAlerts: false,
      connectionDropAlerts: false,
      highUsageWarnings: anyWarningEnabled,
      soundEnabled: nextConfig.notifications?.soundEnabled ?? false,
      warningSettings
    };
    nextConfig.memoryWarningEnabled = warningSettings.memoryEnabled;
    nextConfig.memoryWarningThreshold = warningSettings.memoryThreshold;

    return nextConfig;
  }

  function updateSettingsSaveState() {
    if (!dom.cfgSaveBtn) return;

    if (!settingsBaselineConfig) {
      dom.cfgSaveBtn.disabled = true;
      return;
    }

    const pendingConfig = collectSettingsConfigFromDOM(settingsBaselineConfig);
    dom.cfgSaveBtn.disabled = JSON.stringify(pendingConfig) === JSON.stringify(settingsBaselineConfig);
  }

  function showToast(message, type = 'info') {
    const toast = document.createElement('div');
    toast.className = `toast toast-${type}`;
    toast.textContent = message;
    dom.toastContainer.appendChild(toast);
    requestAnimationFrame(() => toast.classList.add('show'));
    setTimeout(() => {
      toast.classList.remove('show');
      setTimeout(() => toast.remove(), 300);
    }, 3000);
  }

  // ====== Tab System ======
  function switchTab(tabId) {
    state.currentTab = tabId;

    // Update tab buttons
    document.querySelectorAll('.tab-item').forEach(btn => {
      btn.classList.toggle('active', btn.dataset.tab === tabId);
    });

    // Update panels
    document.querySelectorAll('.tab-panel').forEach(panel => {
      panel.classList.remove('active');
    });

    const panelMap = {
      'dashboard': 'panelDashboard',
      'data-usage': 'panelDataUsage',
      'applications': 'panelApps',
      'network': 'panelNetwork',
      'tools': 'panelTools'
    };
    const panel = document.getElementById(panelMap[tabId]);
    if (panel) panel.classList.add('active');

    // Lazy init
    if (!state.tabsInitialized.has(tabId)) {
      state.tabsInitialized.add(tabId);
      initTab(tabId);
    }

    // Refresh data on tab switch
    refreshTab(tabId);
  }

  function initTab(tabId) {
    switch (tabId) {
      case 'dashboard':
        // initTrafficGraph(); // DISABLED - Graph removed
        initDashboardHistoryChart();
        break;
      case 'data-usage':
        initDataUsage();
        initDataUsageHistoryChart();
        break;
      case 'applications':
        initAppMonitor();
        break;
      case 'network':
        initNetworkHealth();
        break;
      case 'tools':
        // No special init needed
        break;
    }
  }

  function refreshTab(tabId) {
    switch (tabId) {
      case 'data-usage':
        loadDataUsage(state.currentDataPeriod);
        break;
      case 'applications':
        loadApplications();
        break;
      case 'network':
        loadNetworkHealth();
        break;
    }
  }

  // ====== Traffic Graph (Task 16) - DISABLED ======
  // function initTrafficGraph() {
  //   if (state.trafficChart || !dom.trafficChart) return;

  //   const ctx = dom.trafficChart.getContext('2d');
  //   const labels = Array.from({ length: CHART_POINTS }, (_, i) => `${CHART_POINTS - i}s`);

  //   state.trafficChart = new Chart(ctx, {
  //     type: 'line',
  //     data: {
  //       labels,
  //       datasets: [
  //         {
  //           label: 'Download',
  //           data: [...state.downloadHistory],
  //           borderColor: '#3b82f6',
  //           backgroundColor: 'rgba(59,130,246,0.08)',
  //           borderWidth: 1.5,
  //           fill: true,
  //           tension: 0.3,
  //           pointRadius: 0
  //         },
  //         {
  //           label: 'Upload',
  //           data: [...state.uploadHistory],
  //           borderColor: '#f97316',
  //           backgroundColor: 'rgba(249,115,22,0.08)',
  //           borderWidth: 1.5,
  //           fill: true,
  //           tension: 0.3,
  //           pointRadius: 0
  //         }
  //       ]
  //     },
  //     options: {
  //       responsive: true,
  //       maintainAspectRatio: false,
  //       animation: { duration: 200 },
  //       scales: {
  //         x: {
  //           display: false
  //         },
  //         y: {
  //           beginAtZero: true,
  //           grid: { color: 'rgba(128,128,128,0.08)' },
  //           ticks: {
  //             font: { size: 9, family: 'JetBrains Mono' },
  //             color: '#888',
  //             callback: (val) => formatSpeedShort(val)
  //           }
  //         }
  //       },
  //       plugins: {
  //         legend: { display: false },
  //         tooltip: { enabled: false }
  //       },
  //       interaction: { intersect: false, mode: 'index' }
  //     }
  //   });
  // }

  function formatSpeedShort(bytesPerSec) {
    if (bytesPerSec === 0) return '0';
    if (bytesPerSec < 1024) return bytesPerSec.toFixed(0) + 'B';
    if (bytesPerSec < 1048576) return (bytesPerSec / 1024).toFixed(0) + 'K';
    return (bytesPerSec / 1048576).toFixed(1) + 'M';
  }

  function updateTrafficGraph(downloadSpeed, uploadSpeed) {
    // DISABLED - Graph removed from dashboard
    // state.downloadHistory.push(downloadSpeed);
    // state.uploadHistory.push(uploadSpeed);
    // if (state.downloadHistory.length > CHART_POINTS) state.downloadHistory.shift();
    // if (state.uploadHistory.length > CHART_POINTS) state.uploadHistory.shift();

    // if (state.trafficChart && state.currentTab === 'dashboard') {
    //   state.trafficChart.data.datasets[0].data = [...state.downloadHistory];
    //   state.trafficChart.data.datasets[1].data = [...state.uploadHistory];
    //   state.trafficChart.update('none');
    // }

    // Track peak for traffic light
    const currentMax = Math.max(downloadSpeed, uploadSpeed);
    if (currentMax > state.peakSpeed) {
      state.peakSpeed = currentMax;
    }
  }

  function updateTrafficLight(downloadSpeed, uploadSpeed) {
    const usage = Math.max(downloadSpeed, uploadSpeed);
    const capacity = state.peakSpeed || (10 * 1024 * 1024); // default 10MB/s
    const pct = (usage / capacity) * 100;

    let color, label, detail;
    if (pct > 80) {
      color = 'red';
      label = 'Heavy';
      detail = 'Network usage is very high';
    } else if (pct > 50) {
      color = 'yellow';
      label = 'Moderate';
      detail = 'Network usage is moderate';
    } else {
      color = 'green';
      label = 'Normal';
      detail = 'Network usage is within normal range';
    }

    if (dom.tlDot) dom.tlDot.className = `tl-dot tl-${color}`;
    if (dom.tlLabel) dom.tlLabel.textContent = label;
    if (dom.tlDetail) dom.tlDetail.textContent = detail;
  }

  // ====== Data Usage (Task 12) ======
  function initDataUsage() {
    // Period selector
    document.querySelectorAll('.du-period-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        document.querySelectorAll('.du-period-btn').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        state.currentDataPeriod = btn.dataset.period;
        loadDataUsage(btn.dataset.period);
      });
    });

    // Data limit save
    dom.duLimitSaveBtn.addEventListener('click', async () => {
      const gb = parseFloat(dom.duLimitInput.value) || 0;
      const bytes = gb * 1024 * 1024 * 1024;
      await window.systemAPI.setDataLimit(bytes);
      showToast(gb > 0 ? `Data limit set to ${gb} GB` : 'Data limit removed', 'info');
      loadDataUsage(state.currentDataPeriod);
    });

    // Init chart
    if (dom.dataUsageChartCanvas) {
      const ctx = dom.dataUsageChartCanvas.getContext('2d');
      state.dataUsageChart = new Chart(ctx, {
        type: 'bar',
        data: {
          labels: [],
          datasets: [
            {
              label: 'Download',
              data: [],
              backgroundColor: 'rgba(59,130,246,0.7)',
              borderRadius: 4
            },
            {
              label: 'Upload',
              data: [],
              backgroundColor: 'rgba(249,115,22,0.7)',
              borderRadius: 4
            }
          ]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          scales: {
            x: {
              grid: { display: false },
              ticks: { font: { size: 9 }, color: '#888', maxRotation: 45 }
            },
            y: {
              beginAtZero: true,
              grid: { color: 'rgba(128,128,128,0.08)' },
              ticks: {
                font: { size: 9, family: 'JetBrains Mono' },
                color: '#888',
                callback: v => formatBytes(v)
              }
            }
          },
          plugins: {
            legend: { display: false },
            tooltip: {
              callbacks: {
                label: ctx => `${ctx.dataset.label}: ${formatBytes(ctx.raw)}`
              }
            }
          }
        }
      });
    }
  }

  async function loadDataUsage(period) {
    try {
      const [usage, thresholds, comparison, historyData] = await Promise.all([
        window.systemAPI.getUsage(period),
        window.systemAPI.getDataThresholds(),
        window.systemAPI.compareUsage(period),
        window.systemAPI.getTrafficHistory(period)
      ]);

      // Summary
      dom.duTotalUsage.textContent = formatBytes(usage.total);
      dom.duDownload.textContent = formatBytes(usage.download);
      dom.duUpload.textContent = formatBytes(usage.upload);

      // Comparison
      if (comparison.percentageChange !== 0) {
        const arrow = comparison.trend === 'up' ? '↑' : comparison.trend === 'down' ? '↓' : '→';
        const cls = comparison.trend === 'up' ? 'trend-up' : comparison.trend === 'down' ? 'trend-down' : 'trend-stable';
        dom.duComparisonValue.textContent = `${arrow} ${Math.abs(comparison.percentageChange)}%`;
        dom.duComparisonValue.className = `du-comparison-value ${cls}`;
        dom.duComparisonTrend.textContent = `Current: ${formatBytes(comparison.current)} / Previous: ${formatBytes(comparison.previous)}`;
      } else {
        dom.duComparisonValue.textContent = 'No comparison available';
        dom.duComparisonValue.className = 'du-comparison-value';
        dom.duComparisonTrend.textContent = '';
      }

      // Limit
      if (thresholds.level !== 'normal' || thresholds.percentage > 0) {
        dom.duLimitFill.style.width = `${Math.min(100, thresholds.percentage)}%`;
        dom.duLimitPct.textContent = `${thresholds.percentage}%`;
        dom.duLimitRemaining.textContent = `${formatBytes(thresholds.remaining)} remaining`;
        
        dom.duLimitFill.className = 'du-limit-fill';
        if (thresholds.level === 'warning') dom.duLimitFill.classList.add('du-fill-warning');
        else if (thresholds.level === 'critical' || thresholds.level === 'exceeded') dom.duLimitFill.classList.add('du-fill-danger');
      } else {
        dom.duLimitFill.style.width = '0%';
        dom.duLimitPct.textContent = '0%';
        dom.duLimitRemaining.textContent = 'No limit set';
        dom.duLimitFill.className = 'du-limit-fill';
      }

      // Chart
      if (state.dataUsageChart && historyData) {
        const recent = historyData.slice(0, 10).reverse();
        state.dataUsageChart.data.labels = recent.map(d => d.date);
        state.dataUsageChart.data.datasets[0].data = recent.map(d => d.download);
        state.dataUsageChart.data.datasets[1].data = recent.map(d => d.upload);
        state.dataUsageChart.update();
      }
    } catch (e) {
      console.error('Failed to load data usage:', e);
    }
  }

  // Dashboard usage summary — lightweight version of loadDataUsage
  async function loadDashUsage(period) {
    try {
      const usage = await window.systemAPI.getUsage(period);
      if (dom.dashTotalUsage) dom.dashTotalUsage.textContent = formatBytes(usage.total);
      if (dom.dashDownload) dom.dashDownload.textContent = formatBytes(usage.download);
      if (dom.dashUpload) dom.dashUpload.textContent = formatBytes(usage.upload);
      if (dom.dashUsagePeriodChip) dom.dashUsagePeriodChip.textContent = getDashPeriodChipLabel(period);
      if (dom.dashUsageMeta) dom.dashUsageMeta.textContent = 'Updated just now';
    } catch (e) {
      console.error('Failed to load dashboard usage:', e);
    }
  }

  // ====== Application Monitor (Task 14) ======

  function initAppMonitor() {
    loadApplications();
    if (state.appPollInterval) clearInterval(state.appPollInterval);
    state.appPollInterval = setInterval(() => {
      if (state.currentTab === 'applications') loadApplications();
    }, 3000);
  }

  function getIconLetter(appName) {
    return appName.charAt(0).toUpperCase();
  }

  function formatSpeedSimple(bytesPerSec) {
    if (bytesPerSec === 0) return '0.0 B/s';
    if (bytesPerSec < 1024) return bytesPerSec.toFixed(1) + ' B/s';
    if (bytesPerSec < 1048576) return (bytesPerSec / 1024).toFixed(1) + ' KB/s';
    return (bytesPerSec / 1048576).toFixed(1) + ' MB/s';
  }

  function formatLastActive(secondsAgo) {
    if (secondsAgo <= 3) return 'Last active now';
    if (secondsAgo < 60) return `Last active ${secondsAgo}s ago`;
    if (secondsAgo < 3600) return `Last active ${Math.floor(secondsAgo / 60)}m ago`;
    return `Last active ${Math.floor(secondsAgo / 3600)}h ago`;
  }

  function getAppStatusLabel(status) {
    switch (status) {
      case 'live':
        return 'Live';
      case 'background':
        return 'Background';
      default:
        return 'Recent';
    }
  }

  function updateAppMonitorStatus(statusInfo = {}) {
    if (!dom.appHeaderSubtitle) return;

    if (statusInfo && statusInfo.requiresAdmin) {
      dom.appHeaderSubtitle.textContent = 'Apps and connection counts are visible now. Live per-app bandwidth unlocks when the app is run as administrator.';
      return;
    }

    if (statusInfo && statusInfo.message && !statusInfo.bandwidthAvailable) {
      dom.appHeaderSubtitle.textContent = statusInfo.message;
      return;
    }

    dom.appHeaderSubtitle.textContent = 'Live now and recent background traffic stay listed here for this session.';
  }

  async function loadApplications() {
    try {
      const [apps, statusInfo] = await Promise.all([
        window.systemAPI.getActiveApplications(),
        window.systemAPI.getAppMonitorStatus()
      ]);
      updateAppMonitorStatus(statusInfo || {});
      renderAppTable(Array.isArray(apps) ? apps : [], statusInfo || {});
    } catch (e) {
      console.error('Failed to load applications:', e);
      dom.appCountBadge.textContent = '0';
      dom.appTableBody.innerHTML = '<tr><td colspan="4" class="app-empty">Unable to load app activity right now.</td></tr>';
    }
  }

  /**
   * Handle ETW network stats from the Go service
   * Feeds data into the same knownApps accumulator for consistent rendering
   */
  function handleETWStats(stats) {
    if (state.currentTab !== 'applications') return;
    if (!stats || !Array.isArray(stats)) return;
    renderAppTable(stats, { bandwidthAvailable: true, requiresAdmin: false });
  }

  /**
   * Render app table from the backend snapshot.
   */
  function renderAppTable(apps = [], statusInfo = {}) {
    const displayApps = apps
      .filter((app) => (app.totalDownload || 0) > 0 || (app.totalUpload || 0) > 0 || (app.connections || 0) > 0);
    const bandwidthAvailable = statusInfo.bandwidthAvailable !== false;

    dom.appCountBadge.textContent = displayApps.length;

    if (displayApps.length === 0) {
      if (statusInfo.requiresAdmin) {
        dom.appTableBody.innerHTML = '<tr><td colspan="4" class="app-empty">Per-app bandwidth needs administrator rights on Windows. The list can still show apps and connections, but live download/upload by app is unavailable until the app is run as administrator.</td></tr>';
      } else {
        dom.appTableBody.innerHTML = '<tr><td colspan="4" class="app-empty">No app network activity yet. Apps that touch the network will stay listed here for this session.</td></tr>';
      }
      return;
    }

    dom.appTableBody.innerHTML = displayApps.map((app, index) => {
      const appStatus = ['live', 'background', 'recent'].includes(app.status) ? app.status : 'recent';
      const appName = escapeHtml(app.name || `Process ${app.pid || ''}`.trim());
      const totalDl = bandwidthAvailable ? formatBytes(app.totalDownload) : '—';
      const totalUl = bandwidthAvailable ? formatBytes(app.totalUpload) : '—';
      const dlSpeedText = bandwidthAvailable ? formatSpeedSimple(app.downloadSpeed) : 'Run as admin';
      const ulSpeedText = bandwidthAvailable ? formatSpeedSimple(app.uploadSpeed) : 'Run as admin';
      const dlSpeedClass = bandwidthAvailable && app.downloadSpeed > 0 ? 'speed-active-down' : 'speed-inactive';
      const ulSpeedClass = bandwidthAvailable && app.uploadSpeed > 0 ? 'speed-active-up' : 'speed-inactive';
      const isIdle = bandwidthAvailable && app.downloadSpeed === 0 && app.uploadSpeed === 0;
      const isHighUsage = bandwidthAvailable && (app.totalDownload + app.totalUpload) > 10485760;
      const statusLabel = escapeHtml(getAppStatusLabel(appStatus));
      const statusClass = `app-status-${appStatus}`;
      const lastActiveText = escapeHtml(formatLastActive(app.lastActiveSeconds || 0));
      const rowClasses = [
        index % 2 !== 0 ? 'app-row-alt' : '',
        isIdle ? 'app-row-idle' : '',
        isHighUsage ? 'app-row-high' : '',
        appStatus === 'recent' ? 'app-row-recent' : ''
      ].filter(Boolean).join(' ');

      return `
        <tr class="${rowClasses}">
          <td class="app-name-cell">
            <span class="app-name-icon">${escapeHtml(getIconLetter(app.name || '?'))}</span>
            <div class="app-name-meta">
              <div class="app-name-topline">
                <span class="app-name-text">${appName}</span>
                <span class="app-status-pill ${statusClass}">${statusLabel}</span>
              </div>
              <div class="app-last-active">${lastActiveText}</div>
            </div>
          </td>
          <td class="app-speed">
            <div class="app-total">${totalDl}</div>
            <div class="app-rate ${dlSpeedClass}">${dlSpeedText}</div>
          </td>
          <td class="app-speed">
            <div class="app-total">${totalUl}</div>
            <div class="app-rate ${ulSpeedClass}">${ulSpeedText}</div>
          </td>
          <td class="app-conns">${app.connections}</td>
        </tr>
      `;
    }).join('');
  }

  // ====== Network Health (Task 13) ======
  function initNetworkHealth() {
    // Speed test button handler is attached in init() to work immediately
    // No need to attach it again here

    // Start polling
    if (state.healthPollInterval) clearInterval(state.healthPollInterval);
    state.healthPollInterval = setInterval(() => {
      if (state.currentTab === 'network') loadNetworkHealth();
    }, 10000);
  }

  async function loadNetworkHealth() {
    try {
      const overview = await window.systemAPI.getNetworkOverview();
      const { health, latency, connection } = overview;

      // Real health snapshot from the Rust backend
      dom.nhQualityDot.style.background = health.color || '#888';
      dom.nhQualityLevel.textContent = health.level || 'Checking';
      dom.nhQualitySub.textContent = health.subtitle || 'Sampling network health';

      dom.nhLatencyValue.textContent = latency.samplesReceived > 0 ? `${latency.latency}ms` : 'Offline';
      dom.nhLatencyLabel.textContent = latency.samplesReceived > 0 ? `${latency.samplesReceived}/${latency.samplesSent} replies from ${latency.target}` : `No replies from ${latency.target}`;
      dom.nhJitterValue.textContent = latency.samplesReceived > 1 ? `${latency.jitter}ms` : '-';
      dom.nhJitterLabel.textContent = latency.samplesReceived > 1 ? `min ${latency.minLatency}ms / max ${latency.maxLatency}ms` : 'Need more than one reply';
      dom.nhLossValue.textContent = `${Math.round(latency.packetLoss)}%`;
      dom.nhLossLabel.textContent = `${latency.samplesSent - latency.samplesReceived} of ${latency.samplesSent} lost`;
      dom.nhConnType.textContent = connection.connectionType === 'wifi' ? 'WiFi' : connection.connectionType === 'ethernet' ? 'Ethernet' : connection.connectionType === 'cellular' ? 'Cellular' : 'Unknown';
      dom.nhConnName.textContent = connection.connectionType === 'wifi' && connection.ssid ? connection.ssid : connection.adapterName || connection.adapterDescription || 'Unavailable';
      dom.nhConnLink.textContent = connection.linkSpeed || 'Unavailable';
      dom.nhConnIp.textContent = connection.localIp || 'Unavailable';
      const bars = connection.bars || 0;
      dom.nhSignalBars.querySelectorAll('.signal-bar').forEach(bar => {
        const barNum = parseInt(bar.dataset.bar, 10);
        bar.classList.toggle('signal-active', barNum <= bars);
      });
      dom.nhSignalBars.style.opacity = connection.connectionType === 'wifi' ? '1' : '0.35';
      dom.nhSignalLabel.textContent = connection.connectionType === 'wifi' ? (connection.percentage > 0 ? `${connection.percentage}% Signal` : 'WiFi') : connection.connectionType === 'ethernet' ? 'Wired Link' : connection.connectionType === 'cellular' ? 'Mobile Data' : 'No link info';





    } catch (e) {
      console.error('Failed to load network health:', e);
    }
  }

  async function updateLatency() {
    if (state.currentTab === 'network') loadNetworkHealth();
  }

  function formatSpeedTestServer(serverLabel, server) {
    const raw = (serverLabel || server || '').toLowerCase();
    if (!raw) return '--';
    if (raw.includes('traffic-monitor-speedtest') || raw.includes('railway.app') || raw.includes('onrender.com') || raw === 'usa') {
      return 'USA · more coming soon';
    }
    if (raw.includes('more coming soon')) return serverLabel || server || '--';
    return serverLabel || server || 'USA · more coming soon';
  }





  async function runSpeedTest() {
    console.log('Speed test button clicked');
    dom.nhSpeedTestBtn.disabled = true;
    dom.nhSpeedTestProgress.style.display = 'flex';
    dom.nhSpeedTestBtn.textContent = 'Running...';

    try {
      console.log('Calling window.systemAPI.runSpeedTest()');
      const result = await window.systemAPI.runSpeedTest();
      console.log('Speed test result:', result);

      dom.nhStDownload.textContent = result.downloadSpeed > 0 
        ? `${result.downloadSpeed.toFixed(2)} MB/s` : 'Error';
      dom.nhStUpload.textContent = result.uploadSpeed > 0 
        ? `${result.uploadSpeed.toFixed(2)} MB/s` : 'Error';
      dom.nhStPing.textContent = result.ping > 0 ? `${result.ping}ms` : '—';
      dom.nhStServer.textContent = formatSpeedTestServer(result.serverLabel, result.server);
      dom.nhStTime.textContent = new Date(result.timestamp).toLocaleString();
      if (result.error) {
        showToast(`Speed test issue: ${result.error}`, 'error');
      } else {
        showToast('Speed test completed successfully', 'info');
      }
    } catch (e) {
      console.error('Speed test error:', e);
      showToast('Speed test failed: ' + e.message, 'error');
    } finally {
      dom.nhSpeedTestBtn.disabled = false;
      dom.nhSpeedTestProgress.style.display = 'none';
      dom.nhSpeedTestBtn.textContent = 'Run Test';
    }
  }

  // ====== Troubleshooter (Task 18) ======
  async function runTroubleshoot() {
    dom.toolsTroubleshootBtn.disabled = true;
    dom.toolsTroubleshootProgress.style.display = 'flex';
    dom.toolsTroubleshootBtn.textContent = 'Running...';
    dom.toolsFixes.style.display = 'none';

    try {
      const results = await window.systemAPI.runDiagnostics();

      // Render results
      dom.toolsTroubleshootResults.innerHTML = results.tests.map(test => {
        const icon = test.status === 'pass' ? '✓' : test.status === 'fail' ? '✗' : '⚠';
        const cls = test.status === 'pass' ? 'test-pass' : test.status === 'fail' ? 'test-fail' : 'test-warn';
        return `
          <div class="test-result ${cls}">
            <span class="test-icon">${icon}</span>
            <div class="test-info">
              <div class="test-name">${test.name}</div>
              <div class="test-msg">${test.message}</div>
            </div>
          </div>
        `;
      }).join('');

      // Render fixes
      if (results.fixes && results.fixes.length > 0) {
        dom.toolsFixes.style.display = 'block';
        dom.toolsFixesList.innerHTML = results.fixes.map(fix => `
          <div class="fix-card">
            <div class="fix-problem">${fix.problem}</div>
            <div class="fix-suggestion">${fix.suggestion}</div>
          </div>
        `).join('');
      }

      showToast(`Diagnostics complete: ${results.overallStatus}`, 
        results.overallStatus === 'pass' ? 'info' : 'error');

    } catch (e) {
      dom.toolsTroubleshootResults.innerHTML = '<div class="tools-empty">Diagnostics failed. Please try again.</div>';
      showToast('Diagnostics failed', 'error');
    } finally {
      dom.toolsTroubleshootBtn.disabled = false;
      dom.toolsTroubleshootProgress.style.display = 'none';
      dom.toolsTroubleshootBtn.textContent = 'Run Diagnostics';
    }
  }

  // ====== Export (Task 19) ======
  function formatExportMonthLabel(monthValue) {
    if (!monthValue || !/^\d{4}-\d{2}$/.test(monthValue)) {
      return monthValue || 'Unknown';
    }

    const [year, month] = monthValue.split('-').map(Number);
    return new Date(year, month - 1, 1).toLocaleDateString('en-US', {
      month: 'long',
      year: 'numeric'
    });
  }

  function setExportStatus(message, type = '') {
    if (!dom.exportStatus) return;

    dom.exportStatus.className = 'export-status';

    if (!message) {
      dom.exportStatus.style.display = 'none';
      dom.exportStatus.textContent = '';
      return;
    }

    dom.exportStatus.style.display = 'block';
    dom.exportStatus.textContent = message;

    if (type === 'success') {
      dom.exportStatus.classList.add('export-success');
    } else if (type === 'error') {
      dom.exportStatus.classList.add('export-error');
    }
  }

  function populateExportSelectOptions(selectEl, values, formatLabel) {
    if (!selectEl) return;

    if (!values || values.length === 0) {
      selectEl.innerHTML = '<option value="">No data yet</option>';
      selectEl.disabled = true;
      return;
    }

    selectEl.disabled = false;
    selectEl.innerHTML = values
      .map(value => `<option value="${value}">${formatLabel(value)}</option>`)
      .join('');
  }

  function refreshExportControls() {
    const period = dom.exportPeriodSelect?.value || 'monthly';
    const usesMonthPicker = period === 'daily' || period === 'weekly';

    if (dom.exportMonthRow) dom.exportMonthRow.hidden = !usesMonthPicker;
    if (dom.exportYearRow) dom.exportYearRow.hidden = usesMonthPicker;

    if (!state.exportAvailability.loaded) {
      if (dom.exportHelper) {
        dom.exportHelper.textContent = 'Loading saved history for export...';
      }
      if (dom.exportBtn) dom.exportBtn.disabled = true;
      return;
    }

    if (!state.exportAvailability.hasHistory) {
      if (dom.exportHelper) {
        dom.exportHelper.textContent = 'No saved history yet. Use the app for a while, then export.';
      }
      if (dom.exportBtn) dom.exportBtn.disabled = true;
      setExportStatus('No saved history is available to export yet.', 'error');
      return;
    }

    if (usesMonthPicker) {
      if (!state.exportAvailability.monthSet.has(dom.exportMonthSelect?.value || '')) {
        dom.exportMonthSelect.value = state.exportAvailability.months[0] || '';
      }
      if (dom.exportHelper) {
        dom.exportHelper.textContent = period === 'daily'
          ? 'Export every saved day from the selected month.'
          : 'Export only the saved weeks from the selected month.';
      }
    } else {
      if (!state.exportAvailability.yearSet.has(dom.exportYearSelect?.value || '')) {
        dom.exportYearSelect.value = state.exportAvailability.years[0] || '';
      }
      if (dom.exportHelper) {
        dom.exportHelper.textContent = period === 'monthly'
          ? 'Export every saved month from the selected year.'
          : 'Export the saved total for the selected year only.';
      }
    }

    if (dom.exportBtn) dom.exportBtn.disabled = false;
  }

  async function loadExportAvailability() {
    try {
      const historyData = await window.systemAPI.getTrafficHistory('daily');
      const dailyHistory = Array.isArray(historyData)
        ? historyData.filter(row => row && typeof row.date === 'string' && row.date.length >= 10)
        : [];

      const monthSet = new Set();
      const yearSet = new Set();

      dailyHistory.forEach(row => {
        const monthKey = row.date.slice(0, 7);
        const yearKey = row.date.slice(0, 4);
        monthSet.add(monthKey);
        yearSet.add(yearKey);
      });

      const months = Array.from(monthSet).sort((a, b) => b.localeCompare(a));
      const years = Array.from(yearSet).sort((a, b) => b.localeCompare(a));

      state.exportAvailability = {
        loaded: true,
        hasHistory: dailyHistory.length > 0,
        months,
        years,
        monthSet,
        yearSet
      };

      populateExportSelectOptions(dom.exportMonthSelect, months, formatExportMonthLabel);
      populateExportSelectOptions(dom.exportYearSelect, years, value => value);
      refreshExportControls();
    } catch (e) {
      console.error('Failed to load export availability:', e);
      state.exportAvailability = {
        loaded: true,
        hasHistory: false,
        months: [],
        years: [],
        monthSet: new Set(),
        yearSet: new Set()
      };
      populateExportSelectOptions(dom.exportMonthSelect, [], value => value);
      populateExportSelectOptions(dom.exportYearSelect, [], value => value);
      refreshExportControls();
    }
  }

  async function handleExport() {
    const period = dom.exportPeriodSelect.value;
    const usesMonthPicker = period === 'daily' || period === 'weekly';
    const month = usesMonthPicker ? (dom.exportMonthSelect?.value || null) : null;
    const year = usesMonthPicker ? null : parseInt(dom.exportYearSelect?.value || '', 10);

    if (usesMonthPicker && !month) {
      setExportStatus('Choose a month with saved history before exporting.', 'error');
      return;
    }

    if (!usesMonthPicker && !Number.isFinite(year)) {
      setExportStatus('Choose a year with saved history before exporting.', 'error');
      return;
    }

    dom.exportBtn.disabled = true;
    dom.exportBtn.textContent = 'Exporting...';
    setExportStatus('Generating report...');

    try {
      const result = await window.systemAPI.exportCSV({
        period,
        month,
        year: Number.isFinite(year) ? year : null
      });

      if (result.success) {
        setExportStatus(`Report saved: ${result.filepath}`, 'success');
        showToast('Report exported successfully', 'info');
      } else {
        setExportStatus(`Export failed: ${result.error}`, 'error');
        showToast('Export failed', 'error');
      }
    } catch (e) {
      setExportStatus('Export failed. Please try again.', 'error');
    } finally {
      dom.exportBtn.disabled = false;
      dom.exportBtn.textContent = 'Download CSV';
      refreshExportControls();
    }
  }



  // ====== Enhanced Settings (Task 20) ======
  function setupTooltips() {
    document.querySelectorAll('[data-tooltip]').forEach(el => {
      let timeoutId = null;
      el.addEventListener('mouseenter', (e) => {
        timeoutId = setTimeout(() => {
          const text = el.getAttribute('data-tooltip');
          dom.globalTooltip.textContent = text;
          dom.globalTooltip.style.display = 'block';
          
          const rect = el.getBoundingClientRect();
          dom.globalTooltip.style.top = `${rect.bottom + 6}px`;
          dom.globalTooltip.style.left = `${rect.left}px`;
          dom.globalTooltip.style.maxWidth = `${Math.min(300, window.innerWidth - rect.left - 20)}px`;
        }, 1000);
      });

      el.addEventListener('mouseleave', () => {
        clearTimeout(timeoutId);
        dom.globalTooltip.style.display = 'none';
      });
    });
  }

  async function updateUndoState() {
    try {
      const canUndo = await window.systemAPI.canUndoSettings();
      dom.cfgUndoBtn.disabled = !canUndo;
    } catch (e) {
      dom.cfgUndoBtn.disabled = true;
    }
  }

  // ====== History Table ======
  async function loadTrafficHistory() {
    try {
      const viewType = dom.historyViewType.value;
      const historyData = await window.systemAPI.getTrafficHistory(viewType);
      
      dom.historyTableBody.innerHTML = '';
      if (!historyData || historyData.length === 0) {
        dom.historyTableBody.innerHTML = '<tr><td colspan="5" style="text-align:center; padding: 20px;">No historical data available.</td></tr>';
        return;
      }

      const maxTotal = Math.max(...historyData.map(d => d.total));

      historyData.forEach(row => {
        const upStr = formatBytes(row.upload);
        const downStr = formatBytes(row.download);
        const totalStr = formatBytes(row.total);
        
        let widthPercent = 0;
        if (maxTotal > 0) {
           widthPercent = (row.total / maxTotal) * 100;
        }
        
        const barClass = widthPercent > 50 ? '' : 'low-usage';

        const tr = document.createElement('tr');
        tr.innerHTML = `
          <td>${row.date}</td>
          <td style="font-family: var(--font-mono)">${upStr}</td>
          <td style="font-family: var(--font-mono)">${downStr}</td>
          <td style="font-family: var(--font-mono); font-weight: 700;">${totalStr}</td>
          <td class="figure-cell">
            <div class="figure-bar ${barClass}" style="width: ${widthPercent}%"></div>
          </td>
        `;
        dom.historyTableBody.appendChild(tr);
      });

    } catch (e) {
      console.error("Failed to load traffic history", e);
    }
  }

  // ====== Metrics Handler ======
  function processMetrics(payload) {
    const { network: stats, cpu, memory } = payload;
    
    // Update network speed display
    const dl = formatSpeed(stats.downloadSpeed);
    const ul = formatSpeed(stats.uploadSpeed);

    dom.downloadSpeed.textContent = dl.value;
    dom.downloadUnit.textContent = dl.unit;
    dom.uploadSpeed.textContent = ul.value;
    dom.uploadUnit.textContent = ul.unit;

    // Update network totals
    if (dom.totalDownloaded) dom.totalDownloaded.textContent = formatBytes(stats.totalDownloaded);
    if (dom.totalUploaded) dom.totalUploaded.textContent = formatBytes(stats.totalUploaded);

    // Update connection status
    state.isConnected = stats.downloadSpeed > 0 || stats.uploadSpeed > 0 || stats.totalDownloaded > 0;
    dom.statusDot.className = 'status-dot ' + (state.isConnected ? 'connected' : 'disconnected');
    dom.connectionStatus.textContent = state.isConnected ? 'Connected' : 'Offline';

    // Update gauges if not hidden
    if (!localConfig.hideGauges) {
      if (cpu) {
        updateGauge(dom.cpuRing, dom.cpuValue, cpu.overall);
        dom.cpuCores.textContent = `${cpu.cores.length} cores`;
      }
      
      if (memory) {
        updateGauge(dom.ramRing, dom.ramValue, memory.percentUsed);
        const usedGB = (memory.active / (1024 * 1024 * 1024)).toFixed(1);
        const totalGB = (memory.total / (1024 * 1024 * 1024)).toFixed(1);
        dom.ramInfo.textContent = `${usedGB} / ${totalGB} GB`;
      }
    }

    // Store current speed for Network Health tab quality assessment
    state.currentDownloadSpeed = stats.downloadSpeed;

    // Update traffic graph (Task 16)
    updateTrafficGraph(stats.downloadSpeed, stats.uploadSpeed);
    updateTrafficLight(stats.downloadSpeed, stats.uploadSpeed);
  }

  async function loadNetworkDetails() {
    if (state.networkDetailsLoaded) return;
    try {
      const interfaces = await window.systemAPI.getNetworkInterfaces();
      const activeIface = interfaces.find(i => i.operstate === 'up' && !i.internal) || interfaces[0];

      if (!activeIface) {
        dom.networkDetailsTable.innerHTML = '<div class="detail-row"><span class="detail-label">No network found</span><span class="detail-value">—</span></div>';
        return;
      }

      const rows = [
        { label: 'Interface', value: activeIface.ifaceName || activeIface.iface },
        { label: 'Type', value: activeIface.type || 'Unknown' },
        { label: 'IP Address', value: activeIface.ip4 || 'N/A' },
        { label: 'Subnet Mask', value: activeIface.ip4subnet || 'N/A' },
        { label: 'IPv6', value: activeIface.ip6 || 'N/A' },
        { label: 'MAC Address', value: activeIface.mac || 'N/A' },
        { label: 'Speed', value: activeIface.speed ? `${activeIface.speed} Mbps` : 'N/A' },
        { label: 'Status', value: activeIface.operstate || 'Unknown' },
        { label: 'DHCP', value: activeIface.dhcp ? 'Yes' : 'No' }
      ];

      dom.networkDetailsTable.innerHTML = rows.map(r => `
        <div class="detail-row">
          <span class="detail-label">${r.label}</span>
          <span class="detail-value" title="${r.value}">${r.value}</span>
        </div>
      `).join('');

      state.networkDetailsLoaded = true;
    } catch (e) {
      dom.networkDetailsTable.innerHTML = '<div class="detail-row"><span class="detail-label">Error loading</span><span class="detail-value">—</span></div>';
    }
  }

  async function loadSystemInfo() {
    if (state.systemInfoLoaded) return;
    try {
      const info = await window.systemAPI.getSystemInfo();

      const rows = [
        { label: 'OS', value: `${info.distro || info.platform} ${info.release || ''}` },
        { label: 'Architecture', value: info.arch || 'N/A' },
        { label: 'Hostname', value: info.hostname || 'N/A' },
        { label: 'CPU', value: info.cpuBrand || 'N/A' },
        { label: 'CPU Cores', value: info.cpuCores || 'N/A' },
        { label: 'CPU Speed', value: info.cpuSpeed ? `${info.cpuSpeed} GHz` : 'N/A' },
        { label: 'Uptime', value: info.uptime ? formatUptime(info.uptime) : 'N/A' }
      ];

      dom.systemInfoTable.innerHTML = rows.map(r => `
        <div class="detail-row">
          <span class="detail-label">${r.label}</span>
          <span class="detail-value" title="${r.value}">${r.value}</span>
        </div>
      `).join('');

      state.systemInfoLoaded = true;

      if (info.uptime) {
        dom.uptimeDisplay.textContent = `Uptime: ${formatUptime(info.uptime)}`;
      }
    } catch (e) {
      dom.systemInfoTable.innerHTML = '<div class="detail-row"><span class="detail-label">Error loading</span><span class="detail-value">—</span></div>';
    }
  }

  // ====== Event Listeners ======
  function setupEventListeners() {
    // Window controls
    dom.minimizeBtn.addEventListener('click', () => window.systemAPI.minimizeWindow());
    dom.closeBtn.addEventListener('click', () => window.systemAPI.closeWindow());

    // Pin button (Taskbar Widget Lock)
    dom.pinBtn.addEventListener('click', () => {
      // Toggle widget lock via state + emit
      state.widgetLocked = !state.widgetLocked;
      dom.pinBtn.classList.toggle('active', state.widgetLocked);
      window.__TAURI__.event.emit('toggle_widget_lock', { locked: state.widgetLocked });
      window.__TAURI__.core.invoke('cmd_toggle_widget_lock', { locked: state.widgetLocked }).catch(() => {});
    });

    window.__TAURI__.event.listen('widget_lock_changed', (event) => {
      state.widgetLocked = event.payload.locked;
      if (dom.pinBtn) dom.pinBtn.classList.toggle('active', state.widgetLocked);
    });

    window.__TAURI__.event.listen('widget-menu-action', async (event) => {
      try {
        await handleWidgetMenuAction(event.payload || {});
      } catch (e) {
        console.error('Failed to handle widget menu action:', e);
      }
    });

    window.__TAURI__.event.listen('widget-feedback', (event) => {
      const payload = event.payload || {};
      if (payload.message) {
        showToast(payload.message, payload.level || 'info');
      }
    });

    // Tab navigation
    dom.tabBar.addEventListener('click', (e) => {
      const btn = e.target.closest('.tab-item');
      if (btn && btn.dataset.tab) {
        switchTab(btn.dataset.tab);
      }
    });

    // Details panel toggles
    dom.networkDetailsToggle.addEventListener('click', () => {
      dom.networkDetailsPanel.classList.toggle('open');
      if (dom.networkDetailsPanel.classList.contains('open')) {
        loadNetworkDetails();
      }
    });

    dom.systemInfoToggle.addEventListener('click', () => {
      dom.systemInfoPanel.classList.toggle('open');
      if (dom.systemInfoPanel.classList.contains('open')) {
        loadSystemInfo();
      }
    });

    if (dom.historyViewType) {
      dom.historyViewType.addEventListener('change', loadTrafficHistory);
    }
    
    if (dom.themeToggleBtn) {
      dom.themeToggleBtn.addEventListener('click', () => {
        document.body.classList.toggle('dark-mode');
        const isDark = document.body.classList.contains('dark-mode');
        localConfig.theme = isDark ? 'dark' : 'light';
        saveLocalConfig();
        updateThemeIcon(isDark);
      });
    }

    // Settings Modal
    dom.settingsBtn.addEventListener('click', async () => {
      dom.settingsModal.classList.add('show');
      await updateUndoState();
      try {
        localConfig = await window.systemAPI.getConfig();
        applyConfigToDOM();
        settingsBaselineConfig = cloneConfig(localConfig);
        applySettingsModalToDOM(settingsBaselineConfig);
        updateSettingsSaveState();
      } catch (e) { /* ignore */ }
    });
    
    dom.settingsCloseBtn.addEventListener('click', () => {
      settingsBaselineConfig = null;
      dom.settingsModal.classList.remove('show');
    });

    const markSettingsDirty = () => {
      updateSettingsSaveState();
    };

    dom.cfgStartOnBoot.addEventListener('change', markSettingsDirty);
    dom.cfgUnitMode.addEventListener('change', markSettingsDirty);
    dom.cfgHideGauges.addEventListener('change', markSettingsDirty);

    [
      dom.cfgWarnTrafficEnabled,
      dom.cfgWarnTrafficThreshold,
      dom.cfgWarnTrafficUnit,
      dom.cfgWarnMemoryEnabled,
      dom.cfgWarnMemoryThreshold,
      dom.cfgWarnCpuTempEnabled,
      dom.cfgWarnCpuTempThreshold,
      dom.cfgWarnGpuTempEnabled,
      dom.cfgWarnGpuTempThreshold,
      dom.cfgWarnDiskTempEnabled,
      dom.cfgWarnDiskTempThreshold,
      dom.cfgWarnMainboardTempEnabled,
      dom.cfgWarnMainboardTempThreshold,
      dom.cfgDataLimit
    ].forEach((el) => {
      if (!el) return;
      el.addEventListener('change', markSettingsDirty);
      if (el.tagName === 'INPUT') {
        el.addEventListener('input', markSettingsDirty);
      }
    });

    dom.cfgSaveBtn.addEventListener('click', async () => {
      try {
        const pendingConfig = collectSettingsConfigFromDOM(settingsBaselineConfig || localConfig);
        localConfig = pendingConfig;
        await saveLocalConfig();
        localConfig = await window.systemAPI.getConfig();
        settingsBaselineConfig = cloneConfig(localConfig);
        applyConfigToDOM();
        applySettingsModalToDOM(localConfig);
        updateSettingsSaveState();
        loadDataUsage(state.currentDataPeriod);
        showToast('Settings saved', 'info');
        await updateUndoState();
      } catch (e) {
        showToast('Failed to save settings', 'error');
      }
    });

    // Recommended settings
    dom.cfgRecommendedBtn.addEventListener('click', async () => {
      try {
        const result = await window.systemAPI.applyRecommendedSettings();
        if (result.success) {
          localConfig = await window.systemAPI.getConfig();
          applyConfigToDOM();
          settingsBaselineConfig = cloneConfig(localConfig);
          applySettingsModalToDOM(localConfig);
          updateSettingsSaveState();
          showToast('Recommended settings applied', 'info');
          await updateUndoState();
        }
      } catch (e) {
        showToast('Failed to apply settings', 'error');
      }
    });

    // Undo settings
    dom.cfgUndoBtn.addEventListener('click', async () => {
      try {
        const result = await window.systemAPI.undoSettings();
        if (result.success) {
          localConfig = await window.systemAPI.getConfig();
          applyConfigToDOM();
          settingsBaselineConfig = cloneConfig(localConfig);
          applySettingsModalToDOM(localConfig);
          updateSettingsSaveState();
          showToast('Settings reverted', 'info');
          await updateUndoState();
        }
      } catch (e) {
        showToast('Failed to undo', 'error');
      }
    });



    // Troubleshooter
    dom.toolsTroubleshootBtn.addEventListener('click', runTroubleshoot);

    dom.exportPeriodSelect.addEventListener('change', () => {
      setExportStatus('');
      refreshExportControls();
    });
    dom.exportMonthSelect.addEventListener('change', () => {
      setExportStatus('');
      refreshExportControls();
    });
    dom.exportYearSelect.addEventListener('change', () => {
      setExportStatus('');
      refreshExportControls();
    });
    dom.exportBtn.addEventListener('click', handleExport);
  }

  function applyConfigToDOM() {
    if (localConfig.hideGauges) {
      dom.gaugesSection.style.display = 'none';
    } else {
      dom.gaugesSection.style.display = 'grid';
    }
    
    // Theme application
    if (localConfig.theme === 'dark') {
      document.body.classList.add('dark-mode');
      updateThemeIcon(true);
    } else {
      document.body.classList.remove('dark-mode');
      updateThemeIcon(false);
    }
  }

  async function saveLocalConfig() {
    return window.systemAPI.saveConfig(localConfig);
  }

  function updateThemeIcon(isDark) {
    if (!dom.themeIcon) return;
    if (isDark) {
      dom.themeIcon.innerHTML = '<circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/>';
    } else {
      dom.themeIcon.innerHTML = '<path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"></path>';
    }
  }

  // ====== In-App Notifications (Task 22.2) ======
  const notificationQueue = [];
  const MAX_VISIBLE_NOTIFICATIONS = 3;
  let notificationCounter = 0;

  /**
   * Display an in-app notification
   * @param {Object} notification - Notification data from main process
   */
  function displayInAppNotification(notification) {
    notificationQueue.push(notification);
    renderNotifications();
  }

  /**
   * Render visible notifications
   */
  function renderNotifications() {
    // Clear existing notifications
    dom.toastContainer.innerHTML = '';

    // Show only the most recent MAX_VISIBLE_NOTIFICATIONS
    const visibleNotifications = notificationQueue.slice(-MAX_VISIBLE_NOTIFICATIONS);

    visibleNotifications.forEach(notif => {
      const notifEl = createNotificationElement(notif);
      dom.toastContainer.appendChild(notifEl);
      
      // Trigger animation
      requestAnimationFrame(() => {
        notifEl.classList.add('show');
      });
    });
  }

  /**
   * Create notification DOM element
   * @param {Object} notif - Notification data
   * @returns {HTMLElement} Notification element
   */
  function createNotificationElement(notif) {
    const notifEl = document.createElement('div');
    notifEl.className = `notification notification-${notif.type}`;
    notifEl.dataset.notificationId = notif.id;

    // Icon based on type
    const iconMap = {
      'info': 'ℹ️',
      'warning': '⚠️',
      'critical': '🚨'
    };
    const icon = iconMap[notif.type] || 'ℹ️';

    // Build notification HTML
    let html = `
      <div class="notification-icon">${icon}</div>
      <div class="notification-content">
        <div class="notification-title">${escapeHtml(notif.title)}</div>
        <div class="notification-message">${escapeHtml(notif.message)}</div>
    `;

    // Add action buttons if present
    if (notif.actions && notif.actions.length > 0) {
      html += '<div class="notification-actions">';
      notif.actions.forEach(action => {
        html += `<button class="notification-action-btn" data-action="${escapeHtml(action.action)}">${escapeHtml(action.label)}</button>`;
      });
      html += '</div>';
    }

    html += `
      </div>
      <button class="notification-close" title="Dismiss">×</button>
    `;

    notifEl.innerHTML = html;

    // Attach event listeners
    const closeBtn = notifEl.querySelector('.notification-close');
    closeBtn.addEventListener('click', () => {
      dismissNotification(notif.id);
    });

    // Action button handlers
    const actionBtns = notifEl.querySelectorAll('.notification-action-btn');
    actionBtns.forEach(btn => {
      btn.addEventListener('click', () => {
        const action = btn.dataset.action;
        handleNotificationAction(notif.id, action);
      });
    });

    // Auto-dismiss after 10 seconds for info, 15 seconds for warning, never for critical
    if (notif.type === 'info') {
      setTimeout(() => dismissNotification(notif.id), 10000);
    } else if (notif.type === 'warning') {
      setTimeout(() => dismissNotification(notif.id), 15000);
    }

    return notifEl;
  }

  /**
   * Dismiss a notification
   * @param {string} notificationId - Notification ID
   */
  function dismissNotification(notificationId) {
    // Remove from queue
    const index = notificationQueue.findIndex(n => n.id === notificationId);
    if (index !== -1) {
      notificationQueue.splice(index, 1);
    }

    // Remove from DOM with animation
    const notifEl = dom.toastContainer.querySelector(`[data-notification-id="${notificationId}"]`);
    if (notifEl) {
      notifEl.classList.remove('show');
      setTimeout(() => {
        notifEl.remove();
      }, 300);
    }

    // Notify main process
    window.systemAPI.dismissNotification(notificationId);
  }

  /**
   * Handle notification action
   * @param {string} notificationId - Notification ID
   * @param {string} action - Action identifier
   */
  function handleNotificationAction(notificationId, action) {
    // Dismiss the notification
    dismissNotification(notificationId);

    // Notify main process
    window.systemAPI.handleNotificationAction(notificationId, action);

    // Handle action locally (switch tabs, etc.)
    if (action === 'view-details' || action === 'view-applications') {
      switchTab('applications');
    } else if (action === 'view-data-usage') {
      switchTab('data-usage');
    } else if (action === 'view-network') {
      switchTab('network');
    }
  }

  /**
   * Escape HTML to prevent XSS
   * @param {string} text - Text to escape
   * @returns {string} Escaped text
   */
  function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
  }

  /**
   * Listen for tab switch events from main process
   */
  function setupNotificationListeners() {
    // Listen for in-app notifications
    window.systemAPI.onNotification((notification) => {
      displayInAppNotification(notification);
    });

    // Listen for tab switch requests from main process
    window.addEventListener('message', (event) => {
      if (event.data && event.data.type === 'switch-tab') {
        switchTab(event.data.tab);
      }
    });
  }

  // ====== Traffic History Chart (Task 31) ======
  
  /**
   * Initialize Dashboard History Chart
   */
  function initDashboardHistoryChart() {
    const canvas = document.getElementById('dashboardHistoryChart');
    if (!canvas) return;

    const statsElements = {
      total: document.getElementById('thStatTotal'),
      upload: document.getElementById('thStatUpload'),
      download: document.getElementById('thStatDownload'),
      dateRange: document.getElementById('thDateRange')
    };

    state.dashboardHistoryChart = new TrafficHistoryChart(canvas, statsElements);
    state.dashboardHistoryChart.initChart();

    // Set up filter buttons
    setupHistoryChartFilters('dashboard');

    // Load default view (today)
    state.dashboardHistoryChart.updateChart('today');
  }

  /**
   * Initialize Data Usage History Chart
   */
  function initDataUsageHistoryChart() {
    const canvas = document.getElementById('dataUsageHistoryChart');
    if (!canvas) return;

    const statsElements = {
      total: document.getElementById('duThStatTotal'),
      upload: document.getElementById('duThStatUpload'),
      download: document.getElementById('duThStatDownload'),
      dateRange: document.getElementById('duThDateRange'),
      dateRangeLabel: document.getElementById('duThDateRangeLabel'),
      monthTrigger: document.getElementById('duThMonthTrigger'),
      monthTriggerLabel: document.getElementById('duThMonthTriggerLabel'),
      monthPickerInput: document.getElementById('duThMonthPicker')
    };

    state.dataUsageHistoryChart = new TrafficHistoryChart(canvas, statsElements);
    state.dataUsageHistoryChart.initChart();

    // Set up filter buttons
    setupHistoryChartFilters('dataUsage');

    // Load default view (Last 7 Days)
    const controls = getHistoryFilterControls('dataUsage');
    setActiveFilter(controls.buttons, controls.last7DaysBtn);
    state.dataUsageHistoryChart.updateChart('last7days');
  }

  /**
   * Set up filter buttons for history charts
   * @param {string} location - 'dashboard' or 'dataUsage'
   */
  function setupHistoryChartFilters(location) {
    const prefix = location === 'dashboard' ? 'th' : 'duTh';
    const chart = location === 'dashboard' ? state.dashboardHistoryChart : state.dataUsageHistoryChart;

    // Get filter buttons
    const todayBtn = document.getElementById(`${prefix}FilterToday`);
    const last7DaysBtn = document.getElementById(`${prefix}FilterLast7Days`);
    const monthlyBtn = document.getElementById(`${prefix}FilterMonthly`);
    const yearlyBtn = document.getElementById(`${prefix}FilterYearly`);
    const monthTrigger = document.getElementById(`${prefix}MonthTrigger`);

    const buttons = [todayBtn, last7DaysBtn, monthlyBtn, yearlyBtn];

    // Add click handlers
    if (todayBtn) {
      todayBtn.addEventListener('click', () => {
        setActiveFilter(buttons, todayBtn);
        chart.updateChart('today');
      });
    }

    if (last7DaysBtn) {
      last7DaysBtn.addEventListener('click', () => {
        setActiveFilter(buttons, last7DaysBtn);
        chart.updateChart('last7days');
      });
    }

    if (monthlyBtn) {
      monthlyBtn.addEventListener('click', () => {
        setActiveFilter(buttons, monthlyBtn);
        const current = getDefaultMonthlyRange(chart);
        chart.updateChart('monthly', current);
      });
    }

    if (yearlyBtn) {
      yearlyBtn.addEventListener('click', () => {
        setActiveFilter(buttons, yearlyBtn);
        const current = getDefaultYearRange(chart);
        chart.updateChart('yearly', current);
        setTimeout(() => {
          showYearPicker(chart, location);
        }, 0);
      });
    }

    if (monthTrigger) {
      monthTrigger.addEventListener('click', async (event) => {
        event.preventDefault();
        event.stopPropagation();

        if (chart.currentFilter === 'yearly') {
          setActiveFilter(buttons, yearlyBtn);
          await chart.updateChart('yearly', getDefaultYearRange(chart));
          setTimeout(() => {
            showYearPicker(chart, location);
          }, 0);
          return;
        }

        if (chart.currentFilter !== 'monthly') {
          setActiveFilter(buttons, monthlyBtn);
          const current = getDefaultMonthlyRange(chart);
          await chart.updateChart('monthly', current);
        }

        setTimeout(() => {
          showMonthPicker(chart, location);
        }, 0);
      });
    }
  }

  /**
   * Set active filter button
   * @param {array} buttons - All filter buttons
   * @param {HTMLElement} activeBtn - Button to set as active
   */
  function setActiveFilter(buttons, activeBtn) {
    buttons.forEach(btn => {
      if (btn) btn.classList.remove('active');
    });
    if (activeBtn) activeBtn.classList.add('active');
  }

  function getDefaultMonthlyRange(chart) {
    const now = new Date();
    return (chart.currentDateRange && chart.currentDateRange.year && chart.currentDateRange.month)
      ? {
          year: chart.currentDateRange.year,
          month: chart.currentDateRange.month
        }
      : {
          year: now.getFullYear(),
          month: now.getMonth() + 1
        };
  }

  function getDefaultYearRange(chart) {
    return {
      year: (chart.currentDateRange && chart.currentDateRange.year) || new Date().getFullYear()
    };
  }

  function getHistoryFilterControls(location) {
    const prefix = location === 'dashboard' ? 'th' : 'duTh';
    const todayBtn = document.getElementById(`${prefix}FilterToday`);
    const last7DaysBtn = document.getElementById(`${prefix}FilterLast7Days`);
    const monthlyBtn = document.getElementById(`${prefix}FilterMonthly`);
    const yearlyBtn = document.getElementById(`${prefix}FilterYearly`);

    return {
      todayBtn,
      last7DaysBtn,
      monthlyBtn,
      yearlyBtn,
      buttons: [todayBtn, last7DaysBtn, monthlyBtn, yearlyBtn]
    };
  }

  async function applyHistoryFilterSelection(location, filter, dateRange = null) {
    const chart = location === 'dashboard' ? state.dashboardHistoryChart : state.dataUsageHistoryChart;
    if (!chart) return;

    const controls = getHistoryFilterControls(location);
    const activeBtn =
      filter === 'today' ? controls.todayBtn :
      filter === 'last7days' ? controls.last7DaysBtn :
      filter === 'monthly' ? controls.monthlyBtn :
      filter === 'yearly' ? controls.yearlyBtn :
      null;

    setActiveFilter(controls.buttons, activeBtn);

    if (filter === 'monthly') {
      await chart.updateChart('monthly', dateRange || getDefaultMonthlyRange(chart));
      return;
    }

    if (filter === 'yearly') {
      await chart.updateChart('yearly', dateRange || getDefaultYearRange(chart));
      return;
    }

    await chart.updateChart(filter);
  }

  async function handleWidgetMenuAction(payload) {
    if (!payload) return;

    if (payload.tab) {
      switchTab(payload.tab);
    }

    if (payload.historyFilter) {
      const dateRange =
        payload.historyFilter === 'monthly' && payload.year && payload.month
          ? { year: payload.year, month: payload.month }
          : null;
      await applyHistoryFilterSelection('dataUsage', payload.historyFilter, dateRange);
    }
  }

  /**
   * Show month picker using Flatpickr
   * @param {TrafficHistoryChart} chart - Chart instance
   * @param {string} location - 'dashboard' or 'dataUsage'
   */
  function showMonthPicker(chart, location) {
    const prefix = location === 'dashboard' ? 'th' : 'duTh';
    const monthlyBtn = document.getElementById(`${prefix}FilterMonthly`);
    const monthTrigger = document.getElementById(`${prefix}MonthTrigger`);
    const monthPickerInput = document.getElementById(`${prefix}MonthPicker`);

    if (!monthlyBtn || !monthTrigger || !monthPickerInput || !window.flatpickr) return;

    // Destroy existing instance if any
    if (chart.flatpickrInstance) {
      chart.flatpickrInstance.destroy();
    }

    const activeDate = chart.currentDateRange
      ? new Date(chart.currentDateRange.year, chart.currentDateRange.month - 1, 1)
      : new Date();

    // Create Flatpickr instance with month selection
    chart.flatpickrInstance = flatpickr(monthPickerInput, {
      clickOpens: false,
      positionElement: monthTrigger,
      plugins: [
        new monthSelectPlugin({
          shorthand: true,
          dateFormat: "M Y",
          altFormat: "F Y"
        })
      ],
      dateFormat: "M Y",
      altFormat: "F Y",
      defaultDate: activeDate,
      onChange: (selectedDates, dateStr, instance) => {
        if (selectedDates.length > 0) {
          const date = selectedDates[0];
          const dateRange = {
            year: date.getFullYear(),
            month: date.getMonth() + 1
          };
          chart.updateChart('monthly', dateRange);
        }
      },
      onClose: () => {
        // Keep the monthly button active
        const buttons = [
          document.getElementById(`${prefix}FilterToday`),
          document.getElementById(`${prefix}FilterLast7Days`),
          document.getElementById(`${prefix}FilterMonthly`),
          document.getElementById(`${prefix}FilterYearly`)
        ];
        setActiveFilter(buttons, monthlyBtn);
      }
    });

    // Open the picker
    chart.flatpickrInstance.open();
  }

  function showYearPicker(chart, location) {
    const prefix = location === 'dashboard' ? 'th' : 'duTh';
    const yearlyBtn = document.getElementById(`${prefix}FilterYearly`);
    const monthTrigger = document.getElementById(`${prefix}MonthTrigger`);
    const monthPickerInput = document.getElementById(`${prefix}MonthPicker`);

    if (!yearlyBtn || !monthTrigger || !monthPickerInput || !window.flatpickr) return;

    if (chart.flatpickrInstance) {
      chart.flatpickrInstance.destroy();
    }

    const activeYear = (chart.currentDateRange && chart.currentDateRange.year) || new Date().getFullYear();
    const activeDate = new Date(activeYear, 0, 1);

    chart.flatpickrInstance = flatpickr(monthPickerInput, {
      clickOpens: false,
      positionElement: monthTrigger,
      defaultDate: activeDate,
      onChange: (selectedDates) => {
        if (selectedDates.length > 0) {
          chart.updateChart('yearly', {
            year: selectedDates[0].getFullYear()
          });
        }
      },
      onClose: () => {
        const buttons = [
          document.getElementById(`${prefix}FilterToday`),
          document.getElementById(`${prefix}FilterLast7Days`),
          document.getElementById(`${prefix}FilterMonthly`),
          document.getElementById(`${prefix}FilterYearly`)
        ];
        setActiveFilter(buttons, yearlyBtn);
      }
    });

    chart.flatpickrInstance.open();
  }

  /**
   * TrafficHistoryChart class for visualizing traffic history with filters
   */
  class TrafficHistoryChart {
    constructor(canvasElement, statsElements) {
      this.canvas = canvasElement;
      this.chart = null;
      this.statsElements = statsElements; // { total, upload, download, dateRange }
      this.currentFilter = 'monthly';
      this.currentDateRange = null;
      this.flatpickrInstance = null;
    }

    /**
     * Initialize Chart.js with stacked bar configuration
     */
    initChart() {
      if (!this.canvas) return;

      const ctx = this.canvas.getContext('2d');
      this.chart = new Chart(ctx, {
        type: 'bar',
        data: {
          labels: [],
          datasets: [
            {
              label: 'Download',
              data: [],
              backgroundColor: '#1e3a8a', // Dark blue
              borderRadius: 0,
              order: 2
            },
            {
              label: 'Upload',
              data: [],
              backgroundColor: '#60a5fa', // Light blue
              borderRadius: 0,
              order: 1
            }
          ]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          animation: {
            duration: 400,
            easing: 'easeInOutQuart'
          },
          scales: {
            x: {
              stacked: true,
              grid: { display: false, color: 'rgba(255,255,255,0.05)' },
              ticks: { 
                font: { size: 9, family: 'JetBrains Mono' }, 
                color: '#888',
                maxRotation: 45,
                minRotation: 0
              }
            },
            y: {
              stacked: true,
              beginAtZero: true,
              grid: { color: 'rgba(255,255,255,0.05)' },
              ticks: {
                font: { size: 9, family: 'JetBrains Mono' },
                color: '#888',
                callback: function(val) {
                  if (val === 0) return '0 B';
                  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
                  const i = Math.floor(Math.log(val) / Math.log(1024));
                  return (val / Math.pow(1024, i)).toFixed(i > 1 ? 2 : 0) + ' ' + units[i];
                }
              }
            }
          },
          plugins: {
            legend: { display: false },
            tooltip: {
              backgroundColor: 'rgba(0,0,0,0.8)',
              titleColor: '#fff',
              bodyColor: '#fff',
              borderColor: 'rgba(255,255,255,0.1)',
              borderWidth: 1,
              padding: 10,
              displayColors: true,
              callbacks: {
                title: (items) => items[0].label,
                label: (context) => {
                  const label = context.dataset.label || '';
                  const val = context.raw;
                  let formatted;
                  if (val === 0) {
                    formatted = '0 B';
                  } else {
                    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
                    const i = Math.floor(Math.log(val) / Math.log(1024));
                    formatted = (val / Math.pow(1024, i)).toFixed(i > 1 ? 2 : 0) + ' ' + units[i];
                  }
                  return `${label}: ${formatted}`;
                },
                footer: (items) => {
                  const total = items.reduce((sum, item) => sum + item.raw, 0);
                  let formatted;
                  if (total === 0) {
                    formatted = '0 B';
                  } else {
                    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
                    const i = Math.floor(Math.log(total) / Math.log(1024));
                    formatted = (total / Math.pow(1024, i)).toFixed(i > 1 ? 2 : 0) + ' ' + units[i];
                  }
                  return `Total: ${formatted}`;
                }
              }
            }
          },
          interaction: { 
            intersect: false, 
            mode: 'index' 
          }
        }
      });
    }

    /**
     * Update chart data based on filter and date range
     * @param {string} filter - Filter type: 'today', 'last7days', 'monthly', 'yearly'
     * @param {object} dateRange - Optional date range for monthly filter
     */
    async updateChart(filter, dateRange = null) {
      if (filter === 'monthly' && !dateRange) {
        dateRange = getDefaultMonthlyRange(this);
      } else if (filter === 'yearly' && !dateRange) {
        dateRange = getDefaultYearRange(this);
      }

      this.currentFilter = filter;
      this.currentDateRange = dateRange;

      try {
        // Map filter to viewType for history.js
        let viewType = 'daily';
        if (filter === 'yearly') {
          viewType = 'monthly';
        } else if (filter === 'monthly' && dateRange) {
          viewType = 'daily';
        } else if (filter === 'last7days') {
          viewType = 'daily';
        } else if (filter === 'today') {
          viewType = 'daily';
        }

        // Get data from history.js
        const historyData = await window.systemAPI.getTrafficHistory(viewType);

        if (!historyData || historyData.length === 0) {
          this.showEmptyState();
          return;
        }

        // Filter data based on selected filter
        let filteredData = this.filterData(historyData, filter, dateRange);

        // Update chart
        if (this.chart && filteredData.length > 0) {
          // Format labels based on filter type
          this.chart.data.labels = filteredData.map(d => this.formatChartLabel(d.date, filter));
          this.chart.data.datasets[0].data = filteredData.map(d => d.download);
          this.chart.data.datasets[1].data = filteredData.map(d => d.upload);
          this.chart.update();

          // Update statistics
          this.updateStatistics(filteredData);

          // Update date range label
          this.updateDateRangeLabel(filter, dateRange);
        }
      } catch (e) {
        console.error('Failed to update traffic history chart:', e);
        this.showEmptyState();
      }
    }

    /**
     * Filter data based on filter type and date range
     * @param {array} data - Raw history data
     * @param {string} filter - Filter type
     * @param {object} dateRange - Optional date range
     * @returns {array} Filtered data
     */
    filterData(data, filter, dateRange) {
      const now = new Date();
      const today = this.getDateString(now);

      if (filter === 'today') {
        return data.filter(d => d.date === today).slice(0, 24).reverse();
      } else if (filter === 'last7days') {
        const dataMap = {};
        data.forEach(d => {
          dataMap[d.date] = d;
        });

        const completeData = [];
        for (let offset = 0; offset <= 7; offset++) {
          const date = new Date(now);
          date.setHours(0, 0, 0, 0);
          date.setDate(date.getDate() - offset);
          const dateStr = this.getDateString(date);

          if (dataMap[dateStr]) {
            completeData.push(dataMap[dateStr]);
          } else {
            completeData.push({
              date: dateStr,
              download: 0,
              upload: 0,
              total: 0
            });
          }
        }

        return completeData;
      } else if (filter === 'monthly' && dateRange) {
        // STRICT RULE: Show ALL days of the month (30 or 31 bars)
        // Even if there's no data for some days, they should show as 0
        
        const year = dateRange.year;
        const month = dateRange.month;
        
        // Get number of days in the selected month
        const daysInMonth = new Date(year, month, 0).getDate();
        
        // Create a map of existing data by date
        const dataMap = {};
        data.forEach(d => {
          dataMap[d.date] = d;
        });
        
        // Build complete array with all days (1 to daysInMonth)
        const completeData = [];
        for (let day = 1; day <= daysInMonth; day++) {
          const dateStr = `${year}-${String(month).padStart(2, '0')}-${String(day).padStart(2, '0')}`;
          
          if (dataMap[dateStr]) {
            // Use existing data
            completeData.push(dataMap[dateStr]);
          } else {
            // Create empty entry for missing day
            completeData.push({
              date: dateStr,
              download: 0,
              upload: 0,
              total: 0
            });
          }
        }
        
        return completeData;
      } else if (filter === 'yearly') {
        const targetYear = String((dateRange && dateRange.year) || now.getFullYear());
        return data.filter(d => d.date.startsWith(targetYear)).slice(0, 12).reverse();
      }

      // Default: return last 30 days
      return data.slice(0, 30).reverse();
    }

    /**
     * Calculate and update statistics
     * @param {array} data - Filtered data
     */
    updateStatistics(data) {
      const stats = this.getStatistics(data);

      if (this.statsElements.total) {
        this.statsElements.total.textContent = formatBytes(stats.total);
      }
      if (this.statsElements.upload) {
        this.statsElements.upload.textContent = formatBytes(stats.upload);
      }
      if (this.statsElements.download) {
        this.statsElements.download.textContent = formatBytes(stats.download);
      }
    }

    /**
     * Format chart label based on filter type
     * @param {string} dateStr - Date string (e.g., "2024-01-15")
     * @param {string} filter - Filter type
     * @returns {string} Formatted label
     */
    formatChartLabel(dateStr, filter) {
      if (filter === 'monthly') {
        // For monthly view, show only day number (e.g., "1", "2", "3", ... "30")
        const date = new Date(dateStr);
        return date.getDate().toString();
      } else if (filter === 'yearly') {
        // For yearly view, show month name (e.g., "January")
        const date = new Date(dateStr);
        const monthNames = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 
                           'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
        return monthNames[date.getMonth()];
      } else if (filter === 'today') {
        // For today view, show hour (e.g., "14:00")
        return dateStr.split(' ')[1] || dateStr;
      } else {
        // For last7days and default, show date (e.g., "Jan 15")
        const date = new Date(dateStr);
        const monthNames = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 
                           'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
        return `${monthNames[date.getMonth()]} ${date.getDate()}`;
      }
    }

    /**
     * Calculate statistics from data
     * @param {array} data - Traffic data
     * @returns {object} Statistics object
     */
    getStatistics(data) {
      const total = data.reduce((sum, d) => sum + d.total, 0);
      const upload = data.reduce((sum, d) => sum + d.upload, 0);
      const download = data.reduce((sum, d) => sum + d.download, 0);

      return { total, upload, download };
    }

    /**
     * Update date range label
     * @param {string} filter - Filter type
     * @param {object} dateRange - Optional date range
     */
    updateDateRangeLabel(filter, dateRange) {
      if (!this.statsElements.dateRange) return;

      const label = this.getDateRangeLabel(filter, dateRange);
      const { dateRange: container, dateRangeLabel, monthTrigger, monthTriggerLabel } = this.statsElements;

      if ((filter === 'monthly' || filter === 'yearly') && monthTrigger && monthTriggerLabel) {
        container.dataset.mode = filter;
        monthTrigger.hidden = false;
        monthTriggerLabel.textContent = label;
        if (dateRangeLabel) {
          dateRangeLabel.hidden = true;
          dateRangeLabel.textContent = label;
        }
        return;
      }

      container.dataset.mode = filter;
      if (monthTrigger) {
        monthTrigger.hidden = true;
      }
      if (dateRangeLabel) {
        dateRangeLabel.hidden = false;
        dateRangeLabel.textContent = label;
      } else {
        container.textContent = label;
      }
    }

    /**
     * Get formatted date range label
     * @param {string} filter - Filter type
     * @param {object} dateRange - Optional date range
     * @returns {string} Formatted label
     */
    getDateRangeLabel(filter, dateRange) {
      const now = new Date();

      if (filter === 'today') {
        return now.toLocaleDateString('en-US', { 
          weekday: 'long', 
          year: 'numeric', 
          month: 'long', 
          day: 'numeric' 
        });
      } else if (filter === 'last7days') {
        const sevenDaysAgo = new Date(now);
        sevenDaysAgo.setDate(sevenDaysAgo.getDate() - 7);
        return `${sevenDaysAgo.toLocaleDateString('en-US', { month: 'short', day: 'numeric' })} - ${now.toLocaleDateString('en-US', { month: 'short', day: 'numeric', year: 'numeric' })}`;
      } else if (filter === 'monthly' && dateRange) {
        const monthNames = ['January', 'February', 'March', 'April', 'May', 'June', 
                           'July', 'August', 'September', 'October', 'November', 'December'];
        return `${monthNames[dateRange.month - 1]} ${dateRange.year}`;
      } else if (filter === 'yearly') {
        return String((dateRange && dateRange.year) || now.getFullYear());
      }

      return 'Select a time period';
    }

    /**
     * Show empty state when no data available
     */
    showEmptyState() {
      if (this.chart) {
        this.chart.data.labels = [];
        this.chart.data.datasets[0].data = [];
        this.chart.data.datasets[1].data = [];
        this.chart.update();
      }

      if (this.statsElements.total) this.statsElements.total.textContent = '0 B';
      if (this.statsElements.upload) this.statsElements.upload.textContent = '0 B';
      if (this.statsElements.download) this.statsElements.download.textContent = '0 B';
      this.updateDateRangeLabel(this.currentFilter, this.currentDateRange);
    }

    /**
     * Get current date string in YYYY-MM-DD format
     * @param {Date} date - Date object
     * @returns {string} Date string
     */
    getDateString(date) {
      const year = date.getFullYear();
      const month = String(date.getMonth() + 1).padStart(2, '0');
      const day = String(date.getDate()).padStart(2, '0');
      return `${year}-${month}-${day}`;
    }

    /**
     * Destroy chart instance
     */
    destroy() {
      if (this.chart) {
        this.chart.destroy();
        this.chart = null;
      }
      if (this.flatpickrInstance) {
        this.flatpickrInstance.destroy();
        this.flatpickrInstance = null;
      }
    }
  }

  // ====== Initialize ======
  async function init() {
    localConfig = await window.systemAPI.getConfig();
    applyConfigToDOM();
    setupEventListeners();
    setupTooltips();
    setupNotificationListeners();
    loadTrafficHistory();
    await loadExportAvailability();

    // Mark dashboard as initialized and init its features
    state.tabsInitialized.add('dashboard');
    // initTrafficGraph(); // DISABLED - Graph removed

    // Dashboard usage summary — period buttons + initial load
    if (dom.dashUsagePeriodChip) {
      dom.dashUsagePeriodChip.addEventListener('click', () => {
        state.dashCurrentPeriod = getNextDashPeriod(state.dashCurrentPeriod);
        loadDashUsage(state.dashCurrentPeriod);
      });
    }
    loadDashUsage(state.dashCurrentPeriod);

    // Hook onto unified metrics pipeline
    window.systemAPI.onMetrics(processMetrics);

    // Listen for ETW network stats (if available on Windows)
    if (window.systemAPI.onETWNetworkStats) {
      window.systemAPI.onETWNetworkStats(handleETWStats);
    }

    // Initial load for things outside the fast loop
    try {
      const disks = await window.systemAPI.getDiskUsage();
      if (disks.length > 0) {
        const mainDisk = disks.find(d => d.mount === 'C:') || disks[0];
        updateGauge(dom.diskRing, dom.diskValue, mainDisk.percentUsed);
        const usedGB = (mainDisk.used / (1024 * 1024 * 1024)).toFixed(0);
        const totalGB = (mainDisk.size / (1024 * 1024 * 1024)).toFixed(0);
        dom.diskInfo.textContent = `${usedGB} / ${totalGB} GB`;
      }
    } catch(e) {}

    // Load last speed test result
    try {
      const history = await window.systemAPI.getSpeedTestHistory();
      if (history && history.length > 0) {
        const last = history[history.length - 1];
        if (dom.nhStDownload) dom.nhStDownload.textContent = last.downloadSpeed > 0 ? `${last.downloadSpeed.toFixed(2)} MB/s` : '—';
        if (dom.nhStUpload) dom.nhStUpload.textContent = last.uploadSpeed > 0 ? `${last.uploadSpeed.toFixed(2)} MB/s` : '—';
        if (dom.nhStPing) dom.nhStPing.textContent = last.ping > 0 ? `${last.ping}ms` : '—';
        if (dom.nhStServer) dom.nhStServer.textContent = formatSpeedTestServer(last.serverLabel, last.server);
        if (dom.nhStTime) dom.nhStTime.textContent = new Date(last.timestamp).toLocaleString();
      }
    } catch(e) {}

    // Attach speed test button handler early so it works even before tab switch
    if (dom.nhSpeedTestBtn) {
      dom.nhSpeedTestBtn.addEventListener('click', runSpeedTest);
    }

    // Update uptime every 30 seconds
    setInterval(async () => {
      try {
        const info = await window.systemAPI.getSystemInfo();
        if (info.uptime) {
          dom.uptimeDisplay.textContent = `Uptime: ${formatUptime(info.uptime)}`;
        }
      } catch (e) { /* silent */ }
    }, 30000);
  }

  // Start when DOM is ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
