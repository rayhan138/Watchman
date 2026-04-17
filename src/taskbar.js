(function () {
  "use strict";

  function waitForTauri(callback) {
    if (window.__TAURI__) {
      callback();
    } else {
      setTimeout(() => waitForTauri(callback), 50);
    }
  }

  document.body.style.webkitAppRegion = "no-drag";

  waitForTauri(() => {
    const { listen } = window.__TAURI__.event;
    const { invoke } = window.__TAURI__.core;

    const dom = {
      widget: document.getElementById("taskbarWidget"),
      uploadRow: document.getElementById("uploadRow"),
      downloadRow: document.getElementById("downloadRow"),
      uploadSpeed: document.getElementById("uploadSpeed"),
      downloadSpeed: document.getElementById("downloadSpeed"),
      cpuValue: document.getElementById("cpuValue"),
      memValue: document.getElementById("memValue"),
    };

    function formatSpeed(bytesPerSec) {
      const value = Number(bytesPerSec || 0);
      if (value < 1024 * 1024) {
        return `${(value / 1024).toFixed(1)} KB/s`;
      }
      if (value < 1024 * 1024 * 1024) {
        return `${(value / (1024 * 1024)).toFixed(1)} MB/s`;
      }
      return `${(value / (1024 * 1024 * 1024)).toFixed(1)} GB/s`;
    }

    function markActive(row, bytesPerSec) {
      if (!row) return;
      row.classList.toggle("active-data", Number(bytesPerSec || 0) > 0);
    }

    function applyWidgetDisplayMode(payload) {
      const networkOnly = !!(payload && payload.networkOnly);
      document.body.dataset.mode = networkOnly ? "network-only" : "full";
    }

    listen("taskbar-placement", (event) => {
      const placement = event.payload || {};
      document.body.dataset.edge = placement.edge || "bottom";
    });

    listen("widget-display-mode-changed", (event) => {
      applyWidgetDisplayMode(event.payload || {});
    });

    listen("metrics", (event) => {
      try {
        const payload = event.payload || {};
        const network = payload.network || {};
        const cpu = payload.cpu || {};
        const memory = payload.memory || {};

        dom.uploadSpeed.textContent = formatSpeed(network.uploadSpeed);
        dom.downloadSpeed.textContent = formatSpeed(network.downloadSpeed);
        dom.cpuValue.textContent = `${Math.round(cpu.overall || 0)}`;
        dom.memValue.textContent = `${Math.round(memory.percentUsed || 0)}`;

        markActive(dom.uploadRow, network.uploadSpeed);
        markActive(dom.downloadRow, network.downloadSpeed);
      } catch (_) {}
    });

    invoke("cmd_get_widget_display_mode")
      .then((payload) => {
        applyWidgetDisplayMode(payload || {});
      })
      .catch(() => {});

    const openWidgetMenu = (event) => {
      event.preventDefault();
      event.stopPropagation();
      invoke("cmd_show_widget_context_menu").catch(() => {});
    };

    dom.widget.addEventListener("dblclick", () => {
      invoke("cmd_show_history_window").catch(() => {});
    });

    dom.widget.addEventListener("contextmenu", openWidgetMenu);
    document.addEventListener("contextmenu", openWidgetMenu);
    document.addEventListener("mousedown", (event) => {
      if (event.button === 2) {
        openWidgetMenu(event);
      }
    });
  });
})();
