# Watchman Code Map

This file explains how the Watchman app is structured, how the frontend and backend connect, and which code sections are responsible for which behavior.

Notes:
- Line numbers are accurate for the current codebase, but they will drift as the project changes.
- The goal of this file is to help you answer: "What does this block do?" and "What does it connect to?"

## 1. High-level architecture

Watchman has four layers:

1. Frontend UI
   - HTML, CSS, and JavaScript under [D:\traficmonitor\traffic-monitor-tauri\src](D:\traficmonitor\traffic-monitor-tauri\src)
   - Renders the main app window and the taskbar widget UI.

2. Tauri bridge
   - [D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js](D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js)
   - Creates `window.systemAPI` so frontend code can call Rust commands.

3. Rust backend
   - [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src)
   - Gathers metrics, saves settings/history, runs diagnostics, emits events, and manages tray/taskbar behavior.

4. Native Windows taskbar layer
   - [D:\traficmonitor\traffic-monitor-tauri\src-tauri\native\taskbar_embed.cpp](D:\traficmonitor\traffic-monitor-tauri\src-tauri\native\taskbar_embed.cpp)
   - Pins the widget onto the Windows taskbar and manages placement.

## 2. Folder structure

### Repo root

- [D:\traficmonitor\traffic-monitor-tauri\package.json](D:\traficmonitor\traffic-monitor-tauri\package.json)
  Bun scripts for frontend + Tauri workflow.
- [D:\traficmonitor\traffic-monitor-tauri\bun.lock](D:\traficmonitor\traffic-monitor-tauri\bun.lock)
  Bun lockfile.
- [D:\traficmonitor\traffic-monitor-tauri\vite.config.js](D:\traficmonitor\traffic-monitor-tauri\vite.config.js)
  Vite config. Builds the frontend into `dist/`.
- [D:\traficmonitor\traffic-monitor-tauri\src](D:\traficmonitor\traffic-monitor-tauri\src)
  Frontend source.
- [D:\traficmonitor\traffic-monitor-tauri\dist](D:\traficmonitor\traffic-monitor-tauri\dist)
  Generated frontend build output.
- [D:\traficmonitor\traffic-monitor-tauri\src-tauri](D:\traficmonitor\traffic-monitor-tauri\src-tauri)
  Tauri + Rust backend.

### `src-tauri` backend folder

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\Cargo.toml](D:\traficmonitor\traffic-monitor-tauri\src-tauri\Cargo.toml)
  Rust package metadata and dependencies.
- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\build.rs](D:\traficmonitor\traffic-monitor-tauri\src-tauri\build.rs)
  Compiles the C++ taskbar code.
- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\tauri.conf.json](D:\traficmonitor\traffic-monitor-tauri\src-tauri\tauri.conf.json)
  Tauri app config.
- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\capabilities\default.json](D:\traficmonitor\traffic-monitor-tauri\src-tauri\capabilities\default.json)
  Window/event/notification permissions.
- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src)
  Rust source.
- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\scripts\temperature_probe.ps1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\scripts\temperature_probe.ps1)
  PowerShell helper used by the temperature backend.
- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\vendor\libre-hardware-monitor](D:\traficmonitor\traffic-monitor-tauri\src-tauri\vendor\libre-hardware-monitor)
  Bundled sensor library used for temperature probing.

## 3. Build/runtime entrypoints

### Frontend build flow

- [D:\traficmonitor\traffic-monitor-tauri\package.json](D:\traficmonitor\traffic-monitor-tauri\package.json:7-18)
  Scripts:
  - `bun run dev`
  - `bun run build`
  - `bun run tauri:dev`
  - `bun run tauri:build`

- [D:\traficmonitor\traffic-monitor-tauri\vite.config.js](D:\traficmonitor\traffic-monitor-tauri\vite.config.js:7-30)
  Vite uses:
  - `src/` as the frontend root
  - `dist/` as the build output
  - `src/index.html` as the main app page
  - `src/taskbar.html` as the widget page

### Tauri runtime flow

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\tauri.conf.json](D:\traficmonitor\traffic-monitor-tauri\src-tauri\tauri.conf.json:7-10)
  Tauri runs:
  - `bun --cwd .. run dev` in development
  - `bun --cwd .. run build` before packaging
  - `http://localhost:1420` in dev mode
  - `../dist` in packaged mode

## 4. Frontend file-by-file guide

### 4.1 Main frontend bootstrap

- [D:\traficmonitor\traffic-monitor-tauri\src\main.js](D:\traficmonitor\traffic-monitor-tauri\src\main.js:1-2)

What it does:
- imports the Tauri bridge
- imports the main renderer

Connection:
- `main.js` does not hold app logic itself
- it ensures `window.systemAPI` exists before `renderer.js` runs

### 4.2 Main app HTML

- [D:\traficmonitor\traffic-monitor-tauri\src\index.html](D:\traficmonitor\traffic-monitor-tauri\src\index.html)

Important sections:

- `22-55`: custom title bar
  - app logo/title
  - settings button
  - theme toggle
  - minimize/close buttons

- `57-83`: top tab bar
  - Dashboard
  - Data
  - Apps
  - Network
  - Tools

- `86-227`: Dashboard panel
  - live upload/download cards
  - usage snapshot
  - traffic health pill
  - CPU/RAM/GPU gauges
  - expandable `Network Details`
  - expandable `System Information`

- `233-332`: Data tab panel
  - usage summary
  - historical chart
  - today / last 7 days / monthly / yearly filters

- `338-364`: Apps tab panel
  - per-app activity table

- `370-470`: Network tab panel
  - connection health
  - latency/jitter/loss
  - connection info
  - speed test

- `476-534`: Tools tab panel
  - diagnostics
  - CSV export

- `539-548`: status bar
  - connection status
  - uptime

- `553-665`: Preferences modal
  - startup/unit/gauge settings
  - warning thresholds
  - version text
  - Use Recommended / Undo / Save

Connection:
- all button IDs and panel IDs here are used by [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js)

### 4.3 Main renderer

- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js)

This is the main frontend brain.

#### `1-374`: shared state, DOM references, helper functions

What lives here:
- global `state`
- `dom` cache for HTML elements
- formatting helpers like `formatSpeed`, `formatBytes`, `formatUptime`
- settings modal helpers

#### `375-793`: settings modal logic

Main anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:375](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:375)

What it does:
- opens the Preferences modal
- loads config from Rust
- pushes values into the modal controls
- collects edited values back from the controls

Connection:
- calls `window.systemAPI.getConfig()`
- later calls `window.systemAPI.saveConfig(...)`
- backend target is [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\config.rs](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\config.rs)

#### `794-1178`: tab switching and panel setup

Main anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:794](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:794)

What it does:
- switches visible tab panels
- lazy-loads tab content
- keeps tab state consistent

#### `1179-1348`: Apps tab loading

Main anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1179](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1179)

What it does:
- requests active application data
- renders the app usage table
- shows empty/loading states

Connection:
- calls `window.systemAPI.getActiveApplications()`
- backend target is [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\app_monitor.rs:867-899](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\app_monitor.rs:867-899)

#### `1349-1697`: Network tab and speed test logic

Key anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1349](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1349)

What it does:
- runs speed tests
- updates network-health UI
- loads speed test history

Connection:
- calls `window.systemAPI.runSpeedTest()`
- calls `window.systemAPI.getSpeedTestHistory()`
- backend target is [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\network_health.rs](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\network_health.rs:194-235)

#### `1698-1746`: live metrics handler

Key anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1698-1746](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1698-1746)

What it does:
- receives one unified metrics payload
- updates:
  - upload/download speed cards
  - status bar connection state
  - CPU/RAM/GPU gauges
  - traffic-light/graph state

Connection:
- called by `window.systemAPI.onMetrics(processMetrics)`
- data is emitted from Rust `metrics_loop`
- backend source is [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1055-1117](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1055-1117)

#### `1748-1782`: dashboard `Network Details`

Key anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1748-1782](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1748-1782)

What it does:
- asks the backend for network interfaces
- chooses the first active non-internal interface
- renders rows like Interface / Type / IP / MAC / Speed / Status

Connection:
- calls `window.systemAPI.getNetworkInterfaces()`
- backend target is [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:1353-1439](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:1353-1439)

Important current limitation:
- the backend currently returns placeholders for `ip4`, `mac`, `type`, and `speed`
- that is why this panel often shows `N/A` and `unknown`

#### `1784-1805`: dashboard `System Information`

Key anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1784-1805](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1784-1805)

What it does:
- loads system summary rows
- renders OS / CPU / host / uptime

Connection:
- calls `window.systemAPI.getSystemInfo()`
- backend target is [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:1370-1444](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:1370-1444)

#### `1878-1965`: event listeners

Key anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1878-1965](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1878-1965)

What it does:
- window buttons
- tray events
- widget menu events
- tab clicks
- details-panel toggles
- theme toggle
- settings modal change tracking

Connection:
- listens for Rust-emitted events like:
  - `widget-menu-action`
  - `widget-feedback`
  - `tray-menu-action`

#### `2796-3228`: `TrafficHistoryChart` class

Key anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:2796-3228](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:2796-3228)

What it does:
- wraps chart rendering for Dashboard/Data history charts
- filters history data by today / last 7 days / monthly / yearly
- updates stats and labels around the chart

Connection:
- calls `window.systemAPI.getTrafficHistory(viewType)`
- backend target is [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\history.rs:203-208](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\history.rs:203-208)

#### `3229-3295`: app initialization

Key anchor:
- [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:3229-3295](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:3229-3295)

What it does:
- loads config
- wires listeners
- loads history/export state
- subscribes to metrics events
- loads last speed test result
- starts periodic uptime refresh

This is the frontend startup entry.

### 4.4 Tauri bridge

- [D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js](D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js)

Important section:
- [D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js:16-83](D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js:16-83)

What it does:
- creates `window.systemAPI`
- maps old Electron-style frontend calls to Tauri `invoke()` calls
- exposes event listeners like `onMetrics`

Examples:
- `getCpuUsage` -> `cmd_get_cpu_usage`
- `getConfig` -> `get_config`
- `getTrafficHistory` -> `get_traffic_history`
- `getNetworkOverview` -> `get_network_overview`
- `runDiagnostics` -> `run_diagnostics`
- `exportCSV` -> `export_csv`

Why it matters:
- this file is the connection layer between your frontend and Rust
- if the frontend says `window.systemAPI.something()`, this is where that mapping lives

### 4.5 Taskbar widget frontend

#### Taskbar HTML

- [D:\traficmonitor\traffic-monitor-tauri\src\taskbar.html](D:\traficmonitor\traffic-monitor-tauri\src\taskbar.html)

What it contains:
- tiny widget layout
- upload and download rows
- CPU and MEM rows
- transparent styling for taskbar embedding

#### Taskbar JS

- [D:\traficmonitor\traffic-monitor-tauri\src\taskbar.js](D:\traficmonitor\traffic-monitor-tauri\src\taskbar.js)

Important anchors:
- `4-14`: wait for Tauri runtime
- `29-41`: format widget speeds
- `44-47`: apply widget display mode
- `49-73`: react to events
- `81-88`: double-click + right-click widget actions

Connection:
- listens for:
  - `taskbar-placement`
  - `widget-display-mode-changed`
  - `metrics`
- invokes:
  - `cmd_get_widget_display_mode`
  - `cmd_show_history_window`
  - `cmd_show_widget_context_menu`

## 5. Backend file-by-file guide

### 5.1 Cargo and Tauri config files

#### `src-tauri/Cargo.toml`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\Cargo.toml:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\Cargo.toml:1)

Important lines:
- `1-10`: Rust package metadata
- `2`: package name is `watchman`
- `3`: app version is `1.0.0`
- `8-10`: library target name is `watchman_lib`
- `12-33`: dependencies

What it does:
- defines the Rust package that Tauri builds
- defines the binary/library names used by Cargo
- lists all Rust dependencies for monitoring, notifications, shell access, Windows APIs, and plugins

Most important dependencies in this project:
- `tauri`: main desktop framework
- `sysinfo`: system usage data
- `reqwest`: HTTP requests for speed test and diagnostics
- `windows`: native Win32 bindings
- `tauri-plugin-notification`: desktop notifications
- `tauri-plugin-single-instance`: prevents duplicate app instances
- `tauri-plugin-window-state`: window state persistence plugin
- `tauri-plugin-autostart`: startup integration
- `cc`: compiles the native C++ taskbar file during build

Connection:
- Cargo reads this file first
- `build.rs` is triggered because Cargo sees it in this crate
- Tauri then builds the app using this dependency graph

#### `src-tauri/build.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\build.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\build.rs:1)

Important lines:
- `1-18`: compile the native C++ widget/taskbar helper

What it does:
- runs before Rust compilation
- compiles `native/taskbar_embed.cpp`
- tells Cargo when to rebuild the native file

Connection:
- `Cargo.toml` includes the `cc` crate
- `build.rs` compiles the C++ code
- `src-tauri/src/taskbar_embed.rs` calls the compiled native symbols

#### `src-tauri/tauri.conf.json`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\tauri.conf.json:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\tauri.conf.json:1)

Important lines:
- `3-5`: product name, version, identifier
- `7-10`: Vite/Bun build integration
- `14-36`: app window configuration
- `37-48`: bundle/resources/icon/NSIS installer settings

What it does:
- tells Tauri how to start the frontend in dev mode
- tells Tauri where the built frontend files live
- defines the app product name `Watchman`
- controls bundle icons, resources, installer mode, and installer icon

Connection:
- `bun run tauri dev` uses `beforeDevCommand` and `devUrl`
- `bun run tauri build` uses `beforeBuildCommand` and `frontendDist`
- Tauri bundle generation uses this file for icons, installer metadata, and windows

#### `src-tauri/capabilities/default.json`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\capabilities\default.json:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\capabilities\default.json:1)

Important lines:
- `3`: capability identifier
- `5`: allows both `main` and `taskbar` windows
- `6-29`: permission list

What it does:
- declares what the frontend is allowed to do
- grants window control, event access, shell usage, notifications, and dialog permissions

Why it matters:
- if the frontend calls a bridge method and Tauri capability blocks it, the feature fails even if Rust code exists

### 5.2 Main backend entry: `src-tauri/src/main.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1)

This file is the real backend hub. It does not contain every monitoring detail, but it connects everything together.

#### `159-420`: warning evaluation and helper functions

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:159](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:159)

What it does:
- checks configured high-usage warning rules
- compares live metrics against thresholds
- fires notifications for traffic, memory, and temperature channels

Connection:
- uses live values from the metrics loop
- uses settings from `ConfigState`
- emits desktop notifications through the notification plugin

#### `422-618`: app startup and Tauri builder

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:422](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:422)

Important lines:
- `446-520`: `invoke_handler(...)`
- `522-560`: taskbar widget webview creation

What it does:
- creates the Tauri app
- registers shared state containers with `.manage(...)`
- registers every backend command the frontend can invoke
- creates the hidden transparent taskbar widget webview
- applies Windows-specific taskbar embedding/styling

The most important `.manage(...)` states here are:
- `MonitorState`
- `ConfigState`
- `HistoryState`
- `AlertRuntimeState`
- `WidgetDisplayState`

These state objects are why different commands can share settings, monitoring snapshots, and history.

Connection:
- frontend bridge methods must match commands listed in `invoke_handler`
- the taskbar window loads `src/taskbar.html`
- native embedding flows through `taskbar_embed::apply_widget_styles(...)` and `taskbar_embed::enforce_widget(...)`

#### `619-760`: tray and window control helpers

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:619](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:619)

Important functions:
- `setup_tray`
- `show_main_window`
- `emit_main_window_action`
- `open_preferences`
- `toggle_widget_visibility`
- `restart_watchman`
- `apply_widget_display_mode`

What they do:
- create the tray icon and its right-click menu
- reopen/focus the main window from tray or widget actions
- tell the frontend to open specific UI states like Preferences or History
- toggle the taskbar widget visibility
- restart the app

Connection:
- tray actions feed into these helpers
- helpers emit frontend events like `open-preferences`
- renderer listeners in [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1878](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1878) react to those events

#### `1055-1128`: metrics and history loops

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1055](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1055)

Important functions:
- `metrics_loop`
- `history_save_loop`

What `metrics_loop` does:
- wakes every second
- asks `MonitorState` for CPU, memory, GPU, network, and temperature readings
- builds a `MetricsPayload`
- records upload/download byte totals into traffic history
- emits a `metrics` event to the frontend
- evaluates warning thresholds

This is the single most important live-data pipeline in the app.

Connection:
- emitted event name is `metrics`
- `src/tauri-bridge.js` exposes `onMetrics`
- `renderer.js` uses `onMetrics(processMetrics)`
- `taskbar.js` also listens to `metrics`

What `history_save_loop` does:
- saves history periodically so traffic totals survive restarts

#### `1221-1328`: widget menu commands

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1221](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1221)

Important commands:
- `cmd_show_history_window`
- `cmd_get_widget_display_mode`
- `cmd_show_widget_context_menu`

What they do:
- open the main window directly into history mode
- tell the widget whether it should be `full` or `network-only`
- show the native Windows popup menu when the user right-clicks the taskbar widget

Connection:
- `taskbar.js` double-click calls `cmd_show_history_window`
- `taskbar.js` right-click calls `cmd_show_widget_context_menu`
- `taskbar.js` startup calls `cmd_get_widget_display_mode`

#### `src-tauri/src/lib.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\lib.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\lib.rs:1)

What it does:
- right now, almost nothing on desktop
- it only contains the mobile entry stub generated by Tauri

Why it matters:
- if you ever add Android/iOS/mobile targets, this file becomes more important
- for the current Windows desktop app, `main.rs` is the real entrypoint you care about

### 5.3 `src-tauri/src/commands` folder

This folder is where the real backend features live. `main.rs` wires them together, but the feature logic mostly sits here.

#### `commands/mod.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\mod.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\mod.rs:1)

What it does:
- declares all backend command modules

Why it matters:
- if a module is not listed here, Rust will not compile it into the crate namespace used by `main.rs`

#### `commands/monitor.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:1)

This is the largest and most important backend file for live monitoring.

Important anchors:
- `851`: `MetricsPayload`
- `865`: `MonitorState`
- `915`: `resolve_temperature_probe_paths`
- `963`: `query_temperature_readings`
- `1032`: `get_cpu_usage`
- `1145`: `get_gpu_usage`
- `1182`: `get_network_stats`
- `1353`: `get_network_interfaces`
- `1370`: `get_system_info`
- `1427`: `cmd_get_temperature_readings`

What each area does:

`MetricsPayload`
- defines the event payload emitted every second
- includes CPU, memory, network, GPU, temperature, and session totals

`MonitorState`
- caches previous samples and counters
- stores values needed for delta-based calculations like bandwidth

`get_cpu_usage`
- uses Windows performance counters first
- falls back to `GetSystemTimes` if needed
- returns total CPU usage used by both dashboard and taskbar widget

`get_gpu_usage`
- reads GPU engine counters from Windows
- identifies the busiest engine such as `3D` or `Video Codec 0`
- returns the displayed overall GPU percentage and active engine label

`get_network_stats`
- reads active adapter byte counters from Windows
- computes download/upload speed from byte deltas over elapsed time
- powers the dashboard speed cards and taskbar widget bandwidth rows

`get_network_interfaces`
- fills the dashboard `Network Details` section
- right now this function still returns placeholder fields for several properties

This is why `Network Details` can show:
- `Type: unknown`
- `IP Address: N/A`
- `MAC Address: N/A`
- `Speed: N/A`

The code path exists, but the real Windows field population is not fully implemented there yet.

`get_system_info`
- returns RAM totals, disk totals, core count, and OS-level hardware info for the `System Information` section

`cmd_get_temperature_readings`
- exposes raw temperature readings when needed

Connection:
- `metrics_loop` depends on this file
- dashboard and taskbar widget both depend on `MetricsPayload`
- `loadNetworkDetails()` calls `get_network_interfaces()`
- `loadSystemInfo()` calls `get_system_info()`

#### `commands/config.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\config.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\config.rs:1)

Important anchors:
- `256`: `ConfigState`
- `275`: `get_config_dir`
- `311`: `get_config`
- `317`: `save_config`
- `358`: `apply_recommended_settings`
- `380`: `undo_settings`
- `402`: `can_undo_settings`

What it does:
- owns app settings
- loads them from disk
- saves changes from the Preferences modal
- supports `Use Recommended` and `Undo`

Important detail:
- this project still stores config under the legacy-compatible folder name `traffic-monitor`
- that was kept on purpose so existing users do not lose saved settings

Connection:
- `renderer.js` settings modal reads with `window.systemAPI.getConfig()`
- save button writes with `window.systemAPI.saveConfig(...)`
- warning thresholds in `main.rs` read from this state

#### `commands/history.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\history.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\history.rs:1)

Important anchors:
- `34`: `HistoryState`
- `46-50`: history path setup, including legacy Electron fallback
- `133`: `get_aggregated_for_export`
- `203`: `get_traffic_history`

What it does:
- stores per-day traffic totals
- loads and saves traffic history JSON
- serves the Data tab charts
- aggregates data for export ranges

Important detail:
- live bandwidth is not stored every second
- only usage totals are persisted into history

Connection:
- `TrafficHistoryChart` calls `get_traffic_history`
- export uses `get_aggregated_for_export`
- `metrics_loop` writes usage totals through this state

#### `commands/network_health.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\network_health.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\network_health.rs:1)

Important anchors:
- `18`: friendly speed-test server label
- `194`: `run_speed_test`
- `235`: `get_speed_test_history`

What it does:
- calculates connection-health summary values for the Network tab
- runs the speed test
- stores the last speed test result
- returns server label as `USA · more coming soon`

Connection:
- Network tab reads this through bridge calls like `getNetworkOverview()` and `runSpeedTest()`

#### `commands/app_monitor.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\app_monitor.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\app_monitor.rs:1)

Important anchors:
- `280`: tracker startup
- `867`: `get_active_applications`
- `881`: `get_app_monitor_status`
- `899`: `terminate_application`

What it does:
- tracks active applications and their network activity
- feeds the Apps tab
- reports whether per-app monitoring is available

Important behavior:
- per-app bandwidth on Windows may require admin mode, depending on the trace/session path in use

Connection:
- `loadApplications()` calls `getActiveApplications()`
- Apps tab also checks app-monitor status through the bridge

#### `commands/data_usage.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\data_usage.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\data_usage.rs:1)

Important anchors:
- `33`: `get_usage`
- `61`: `set_data_limit`
- `73`: `get_remaining_allowance`
- `88`: `get_data_thresholds`
- `123`: `compare_usage`

What it does:
- handles monthly usage limits and allowance math
- powers the usage-comparison logic shown around Data/Tools flows

#### `commands/export.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\export.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\export.rs:1)

Important anchors:
- `6`: `ExportResult`
- `15`: `ExportOptions`
- `22`: `export_csv`

What it does:
- exports traffic history to CSV
- uses the selected period and selected month/year constraints from the Tools tab

Important detail:
- PDF export was intentionally removed
- the app is now honestly CSV-only

#### `commands/troubleshooter.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\troubleshooter.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\troubleshooter.rs:1)

Important anchors:
- `34`: diagnostics result structures
- `57`: `run_diagnostics`

What it does:
- runs the Tools tab network checks
- tests DNS, default route/gateway behavior, and internet connectivity

#### `commands/profile.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\profile.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\profile.rs:1)

Important anchors:
- `100`: `get_profiles`
- `108`: `get_active_profile`
- `126`: `set_active_profile`
- `139`: `get_profile_config`

What it does:
- stores reusable settings profiles
- allows the frontend to switch between them

#### `commands/notifications.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\notifications.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\notifications.rs:1)

Important anchors:
- `25`: `dismiss_notification`
- `30`: `notification_action`

What it does:
- records notification interactions and notification action callbacks

### 5.4 Native Windows taskbar code

#### `src-tauri/src/taskbar_embed.rs`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\taskbar_embed.rs:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\taskbar_embed.rs:1)

Important anchors:
- `19-27`: external C function declarations
- `52`: `apply_widget_styles`
- `57`: `enforce_widget`
- `90`: `restore_taskbar_layout`
- `111`: `should_widget_be_visible`

What it does:
- exposes safe Rust wrappers around the compiled C++ taskbar code
- lets `main.rs` call the native embedding functions without writing unsafe Win32 logic everywhere

#### `src-tauri/native/taskbar_embed.cpp`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\native\taskbar_embed.cpp:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\native\taskbar_embed.cpp:1)

Important anchors:
- `88`: `InitTaskbarWindows`
- `139`: `CaptureOriginalLayout`
- `159`: `RestoreTaskbarLayoutInternal`
- `178`: `IsFullscreenWindow`
- `212`: `ApplyModernPlacement`
- `287`: `tm_apply_widget_styles`
- `314`: `tm_embed_widget`
- `415`: `tm_restore_taskbar_layout`
- `419`: `tm_should_widget_be_visible`

What it does:
- talks directly to Windows taskbar windows
- positions the widget inside/near the taskbar area
- restores layout when needed
- hides the widget during fullscreen cases

Connection:
- `build.rs` compiles this file
- `taskbar_embed.rs` imports the compiled symbols
- `main.rs` calls the Rust wrappers during setup and widget placement updates

### 5.5 Temperature backend path

#### `src-tauri/scripts/temperature_probe.ps1`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\scripts\temperature_probe.ps1:1](D:\traficmonitor\traffic-monitor-tauri\src-tauri\scripts\temperature_probe.ps1:1)

Important anchors:
- `1-6`: parameter block
- `8`: `Update-HardwareNode`
- `17`: `Add-TemperatureSample`
- `39`: `Get-PreferredTemperature`
- `123`: output as compressed JSON

What it does:
- loads LibreHardwareMonitor through PowerShell
- reads CPU, GPU, disk, and mainboard temperature sensors
- returns compact JSON back to Rust

Connection:
- `monitor.rs` resolves the script/resource paths
- `monitor.rs` executes the script and parses its JSON
- `main.rs` warning evaluation uses those temperature readings

#### `src-tauri/vendor/libre-hardware-monitor`

- [D:\traficmonitor\traffic-monitor-tauri\src-tauri\vendor\libre-hardware-monitor](D:\traficmonitor\traffic-monitor-tauri\src-tauri\vendor\libre-hardware-monitor)

What it contains:
- the bundled LibreHardwareMonitor assemblies used by the temperature probe

Why it matters:
- the PowerShell script does not invent temperature readings itself
- it loads this bundled hardware-monitoring library, reads sensors, and then sends the results back to Rust

Important limitation:
- on some Windows machines, full sensor access may require administrator mode
- that is why the app now shows the small settings note about admin mode for temperature warnings

## 6. Where the app stores data

### Config and history

Main storage paths:
- `C:\Users\<your-user>\AppData\Roaming\traffic-monitor\settings.json`
- `C:\Users\<your-user>\AppData\Roaming\traffic-monitor\traffic_history.json`

Legacy fallback path still recognized:
- `C:\Users\<your-user>\AppData\Roaming\electron-app\traffic_history.json`

Why the folder still says `traffic-monitor`:
- the app branding changed to `Watchman`
- but the storage folder name was intentionally left alone to avoid breaking existing user data

### What is stored and what is not

Stored:
- settings
- warning thresholds
- history totals per day
- last speed test result and related saved state

Not stored as permanent history:
- every live per-second bandwidth tick
- every live CPU percentage sample
- every live GPU percentage sample

That live data is computed in real time from Windows counters and system queries.

## 7. Most important frontend-backend connections

### Connection 1: live metrics

Flow:
1. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1055](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1055) runs `metrics_loop`
2. it collects values from [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:851](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:851)
3. it emits the `metrics` event
4. [D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js:78](D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js:78) exposes `onMetrics`
5. [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1698](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1698) handles it in `processMetrics`
6. [D:\traficmonitor\traffic-monitor-tauri\src\taskbar.js:58](D:\traficmonitor\traffic-monitor-tauri\src\taskbar.js:58) also listens and updates the taskbar widget

This one flow powers:
- dashboard live speeds
- dashboard CPU/RAM/GPU cards
- taskbar widget numbers
- live session counters
- warning checks

### Connection 2: settings save

Flow:
1. user edits the Preferences modal in `renderer.js`
2. save button calls `window.systemAPI.saveConfig(...)`
3. [D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js:28](D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js:28) maps that to `save_config`
4. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\config.rs:317](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\config.rs:317) writes `settings.json`
5. `main.rs` and warning logic then use the updated settings state

### Connection 3: Data tab history

Flow:
1. `TrafficHistoryChart` decides which view the user selected
2. it calls `window.systemAPI.getTrafficHistory(viewType)`
3. the bridge maps that to `get_traffic_history`
4. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\history.rs:203](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\history.rs:203) returns aggregated history data
5. the chart and totals cards render from that response

### Connection 4: dashboard `Network Details`

Flow:
1. user opens the `Network Details` accordion
2. [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1748](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js:1748) calls `window.systemAPI.getNetworkInterfaces()`
3. the bridge maps it to `get_network_interfaces`
4. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:1353](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs:1353) builds the interface objects
5. renderer prints each field to the accordion rows

Important note:
- this path is currently only partially implemented
- that is why some fields show `N/A` or `unknown`

### Connection 5: taskbar widget right-click menu

Flow:
1. user right-clicks the taskbar widget
2. [D:\traficmonitor\traffic-monitor-tauri\src\taskbar.js:85](D:\traficmonitor\traffic-monitor-tauri\src\taskbar.js:85) blocks the browser menu and calls `cmd_show_widget_context_menu`
3. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1249](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs:1249) shows the native Windows popup menu
4. menu actions call helpers like `show_main_window`, `open_preferences`, `toggle_widget_visibility`, and `restart_watchman`
5. some actions emit events back to the main window frontend

## 8. Files to read first if you want to understand the app quickly

If you want the shortest path to understanding the project, read in this order:

1. [D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js](D:\traficmonitor\traffic-monitor-tauri\src\tauri-bridge.js)
2. [D:\traficmonitor\traffic-monitor-tauri\src\renderer.js](D:\traficmonitor\traffic-monitor-tauri\src\renderer.js)
3. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\main.rs)
4. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\monitor.rs)
5. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\config.rs](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\config.rs)
6. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\history.rs](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\commands\history.rs)
7. [D:\traficmonitor\traffic-monitor-tauri\src\taskbar.js](D:\traficmonitor\traffic-monitor-tauri\src\taskbar.js)
8. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\taskbar_embed.rs](D:\traficmonitor\traffic-monitor-tauri\src-tauri\src\taskbar_embed.rs)
9. [D:\traficmonitor\traffic-monitor-tauri\src-tauri\native\taskbar_embed.cpp](D:\traficmonitor\traffic-monitor-tauri\src-tauri\native\taskbar_embed.cpp)

## 9. Current limitations and important notes

- `Network Details` backend is not fully populated yet, so some rows still show placeholders
- some temperature sensors may require running the app as administrator
- taskbar widget behavior depends on Windows-specific native code and can behave differently on different taskbar layouts
- the project uses modern Bun + Vite + Tauri structure now, but some backend storage names still use legacy `traffic-monitor` paths for compatibility

## 10. If you want this doc to become even stronger later

The next improvements that would make this file even more useful are:
- add a Mermaid diagram for the metrics flow
- add a second doc only for `renderer.js`
- add a second doc only for `main.rs` + `monitor.rs`
- annotate the placeholder parts that still need backend completion, especially `Network Details`
