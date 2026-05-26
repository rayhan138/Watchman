# Watchman

Watchman is a Windows desktop app for live network monitoring, traffic history,
per-app bandwidth, network health checks, system gauges, and a compact taskbar
widget.

The app is built with Tauri 2, Rust, and a plain HTML/CSS/JavaScript frontend.

## Project Structure

```text
watchman/
|-- package.json             Frontend and Tauri scripts
|-- vite.config.js           Vite multi-page build config
|-- bun.lock                 Locked frontend toolchain dependencies
|-- src/                     Frontend source
|   |-- index.html           Main app window
|   |-- taskbar.html         Taskbar widget window
|   |-- main.js              Frontend entrypoint
|   |-- renderer.js          Main app UI behavior
|   |-- taskbar.js           Widget UI behavior
|   |-- tauri-bridge.js      Browser-to-Tauri command bridge
|   |-- styles.css           App styling
|   `-- watchman-logo.svg    App logo
|-- src-tauri/               Rust and Tauri backend
|   |-- Cargo.toml           Rust package metadata
|   |-- tauri.conf.json      Tauri app configuration
|   |-- build.rs             Native build integration
|   |-- capabilities/        Tauri permissions
|   |-- icons/               App icons
|   |-- native/              Windows native integration
|   |-- scripts/             Runtime helper scripts
|   |-- vendor/              Bundled third-party runtime libraries
|   `-- src/                 Rust source
|-- brand/                   Brand and product artwork
|-- design/                  Design references and mockups
`-- docs/                    Architecture and maintenance notes
```

Generated folders such as `dist/`, `builds/`, `node_modules/`, and
`src-tauri/target/` are ignored and should not be committed.

## Development

Install dependencies:

```bash
bun install
```

Run the app in development:

```bash
bun run tauri:dev
```

Build the frontend only:

```bash
bun run build
```

Build the desktop app:

```bash
bun run tauri:build
```

## Runtime Data

Watchman stores user data locally under the Windows roaming app data folder:

```text
%APPDATA%\Watchman\settings.json
%APPDATA%\Watchman\history.json
```

Older app-data paths are not used as active Watchman storage.

## Notes for Maintainers

- Keep product-facing names as `Watchman`.
- Keep generated release artifacts out of source control.
- Treat `src-tauri/vendor/libre-hardware-monitor/` as a bundled runtime
  dependency for temperature readings.
- The taskbar widget has a native Windows layer in `src-tauri/native/`; change
  it only when specifically working on widget embedding behavior.
