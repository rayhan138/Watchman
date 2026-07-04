

<img width="600" height="200" alt="Monitor your bandwidth" src="https://github.com/user-attachments/assets/129137cc-38f1-4f77-bb13-f30b0d50d0c9" />






# Watchman

Watchman is a lightweight Windows network monitor that helps you see what your PC is doing with your internet in real time.

It shows live download and upload speed, per-app bandwidth activity, traffic history, connection health, system usage, and a compact taskbar widget in one clean desktop app.

[Official Website](https://watchman.blinkeye.app/)  
[Download Latest Release](https://github.com/rayhan138/Watchman/releases/latest)  
[Join our Discord](https://discord.gg/RSrx4zs2Hh)

## Screenshots

<img width="1920" height="600" alt="Untitled design (62)" src="https://github.com/user-attachments/assets/35214cd2-1dcc-494c-894c-d68bbc74a0dd" />


---

## Why Watchman?

Sometimes your internet feels slow, but it is not always clear what is using it in the background.

Watchman was built to make that easier to understand. It gives you a simple place to see your current network speed, which apps are active, how much data you have used, and whether your connection looks stable.

The goal is not to be a heavy network tool. Watchman is made to be small, useful, and easy to keep open while you work.

---

## Features

- Live download and upload speed
- Per-app network activity and connection count
- Daily, weekly, monthly, and yearly traffic history
- CPU, RAM, and GPU usage gauges
- Connection health with latency, jitter, and packet loss
- M-Lab speed test
- CSV export for saved traffic history
- Compact taskbar widget
- Light, dark, and focus themes
- Local-first data storage
- Update checking
- Feedback button

---

## Download

Download the latest Windows installer from the GitHub Releases page:

[Download Watchman](https://github.com/rayhan138/Watchman/releases/latest)

After installing, Watchman opens as a small desktop app and can also show a compact taskbar widget for quick monitoring.

---

## Local Data Storage

Watchman stores traffic history locally on your PC.

Main history database:

```text
%APPDATA%\Watchman\watchman.db
```

Example Windows path:

```text
C:\Users\<your-name>\AppData\Roaming\Watchman\watchman.db
```

Watchman uses a local SQLite database for saved traffic history. This data is used for the Data tab, charts, and CSV export.

Your traffic history stays on your own computer.

---

## Privacy

Watchman is designed as a local-first desktop app.

- No account is required
- No cloud dashboard is required
- Traffic history is stored locally
- The app does not need your personal login data
- Update checks only contact the GitHub release channel
- The feedback button opens an external feedback form only when you click it

Some Windows network information may come from system commands or Windows APIs. Watchman uses this information to show connection type, adapter details, speed, and connection health inside the app.

---

## Development Setup

### Prerequisites

Install these before running the app locally:

- Node.js
- Rust
- Cargo
- Tauri prerequisites for Windows
- Bun or npm

### Install Dependencies

```powershell
npm install
```

### Run in Development Mode

```powershell
npm run tauri:dev
```

### Build the App

```powershell
npm run tauri:build
```

Build output is created by Tauri under:

```text
src-tauri\target\release\bundle\
```

---

## Contributing

Contributions are welcome.

You can help by:

- Reporting bugs
- Suggesting improvements
- Improving the UI
- Testing Watchman on different Windows versions
- Improving documentation

Basic contribution flow:

```powershell
git clone https://github.com/rayhan138/Watchman.git
cd Watchman
git checkout -b feature/your-change
npm install
npm run tauri:dev
```

After making changes, commit and push your branch, then open a pull request.

Please keep changes focused and explain what problem your pull request solves.

---

## Security

If you find a security issue, please do not open a public GitHub issue with sensitive details.

Report it privately instead.

Suggested report details:

- What happened
- Steps to reproduce
- Your Windows version
- Watchman version
- Any relevant screenshots or logs

---

## License

Watchman is licensed under the terms included in the repository license file.

See:

[LICENSE](./LICENSE.md)
