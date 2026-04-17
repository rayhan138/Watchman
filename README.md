# Watchman

Watchman is a Tauri desktop app for network monitoring, traffic history, per-app bandwidth, taskbar widget stats, and system health.

## Project structure

```text
watchman/
├─ package.json          # Bun scripts + frontend tooling
├─ vite.config.js        # Vite multi-page frontend config
├─ src/                  # Frontend source (HTML, JS, CSS, assets)
│  ├─ index.html         # Main app window entry
│  ├─ taskbar.html       # Taskbar widget entry
│  ├─ main.js            # Frontend bootstrap
│  ├─ taskbar.js         # Taskbar widget bootstrap
│  ├─ renderer.js        # Main app UI logic
│  ├─ tauri-bridge.js    # window.systemAPI compatibility bridge
│  ├─ styles.css         # Main UI styles
│  └─ watchman-logo.svg  # App logo
├─ dist/                 # Vite build output (generated)
└─ src-tauri/            # Rust/Tauri backend
   ├─ Cargo.toml
   ├─ tauri.conf.json
   └─ src/
```

## Development

Install frontend tooling:

```bash
bun install
```

Run the full Tauri app in development:

```bash
bun run tauri dev
```

Build the frontend only:

```bash
bun run build
```

Build the desktop app:

```bash
bun run tauri build
```
