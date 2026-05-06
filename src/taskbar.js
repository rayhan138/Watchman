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

    function applyTaskbarTheme(payload) {
      const isLight = !!(payload && payload.isLight);
      document.body.dataset.taskbarTheme = isLight ? "light" : "dark";
    }

    function refreshTaskbarTheme() {
      invoke("cmd_get_taskbar_theme")
        .then((payload) => {
          applyTaskbarTheme(payload || {});
        })
        .catch(() => {
          applyTaskbarTheme({ isLight: false });
        });
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

    refreshTaskbarTheme();
    setInterval(refreshTaskbarTheme, 5000);

    let lastContextMenuAt = 0;
    let lastHistoryOpenAt = 0;
    let lastLeftClick = null;

    function consumePointerEvent(event) {
      event.preventDefault();
      event.stopPropagation();
      if (typeof event.stopImmediatePropagation === "function") {
        event.stopImmediatePropagation();
      }
    }

    function openWidgetMenu(event) {
      if (event) {
        if (event.__watchmanMenuHandled) return;
        event.__watchmanMenuHandled = true;
        consumePointerEvent(event);
      }

      const now = Date.now();
      if (now - lastContextMenuAt < 350) return;
      lastContextMenuAt = now;
      invoke("cmd_show_widget_context_menu").catch(() => {});
    }

    function openHistoryWindow(event) {
      if (event) {
        consumePointerEvent(event);
      }

      const now = Date.now();
      if (now - lastHistoryOpenAt < 450) return;
      lastHistoryOpenAt = now;
      invoke("cmd_show_history_window").catch(() => {});
    }

    function isRightButton(event) {
      return event.button === 2 || (event.buttons & 2) === 2;
    }

    function handleRightButtonDown(event) {
      if (!isRightButton(event)) return;
      consumePointerEvent(event);
    }

    function handleRightButtonUp(event) {
      if (event.button !== 2) return;
      openWidgetMenu(event);
    }

    function handleLeftButtonUp(event) {
      if (event.__watchmanLeftHandled || event.button !== 0) return;
      event.__watchmanLeftHandled = true;

      const now = Date.now();
      const click = {
        x: event.clientX,
        y: event.clientY,
        time: now,
      };

      if (
        lastLeftClick &&
        now - lastLeftClick.time < 360 &&
        Math.abs(click.x - lastLeftClick.x) <= 8 &&
        Math.abs(click.y - lastLeftClick.y) <= 8
      ) {
        lastLeftClick = null;
        openHistoryWindow(event);
        return;
      }

      lastLeftClick = click;
    }

    const eventTargets = [
      window,
      document,
      document.documentElement,
      document.body,
      dom.widget,
    ].filter(Boolean);

    eventTargets.forEach((target) => {
      target.addEventListener("contextmenu", openWidgetMenu, true);
      target.addEventListener("pointerdown", handleRightButtonDown, true);
      target.addEventListener("pointerup", handleRightButtonUp, true);
      target.addEventListener("mouseup", handleRightButtonUp, true);
      target.addEventListener("pointerup", handleLeftButtonUp, true);
      target.addEventListener("dblclick", openHistoryWindow, true);
      target.addEventListener("auxclick", (event) => {
        if (isRightButton(event)) openWidgetMenu(event);
      }, true);
    });
  });
})();
