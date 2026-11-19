

<img width="600" height="200" alt="Monitor your bandwidth" src="https://github.com/user-attachments/assets/129137cc-38f1-4f77-bb13-f30b0d50d0c9" />






# Watchman

**2.0.1 publisher hotfix candidate:** The app in `native/` is C++/Win32. The candidate sets the Windows publisher and executable CompanyName to **Noman Dhoni** while retaining the existing install identity and user data. [Migration notes and acceptance evidence](migration/README.md). The stable release link remains on v2.0.0 until both independent Windows upgrade tests pass.

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
- Light, dark, and Windows system themes
- Local-first data storage
- Update checking

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
%APPDATA%\Watchman.Native\watchman.db
```

Example Windows path:

```text
C:\Users\<your-name>\AppData\Roaming\Watchman.Native\watchman.db
```

Watchman uses a local SQLite database for saved traffic history. This data is used for the Data tab, charts, and CSV export.

The 2.0.0 migration keeps SQLite history from v1 and earlier native builds. Without a database it starts empty; it never imports `history.json` or `history.json.bak`. Those files are retained, and settings migration is unchanged. A completed native profile is not reset.

---

## Privacy

Watchman is designed as a local-first desktop app.

- No account is required
- No cloud dashboard is required
- Traffic history is stored locally
- The app does not need your personal login data
- Update checks only contact the GitHub release channel

Some Windows network information may come from system commands or Windows APIs. Watchman uses this information to show connection type, adapter details, speed, and connection health inside the app.

---

## Development Setup

On x64 Windows, install Visual Studio 2022 Desktop development with C++, the Windows SDK, and CMake. Build the native executable and tests:

```powershell
cmake -S native -B builds/native -A x64
cmake --build builds/native --config Release
ctest --test-dir builds/native -C Release --output-on-failure
```

For the complete installer, run `./migration/Build-Native.ps1` under PowerShell 7 with Rust/Cargo, Node/npm and network access for packaging tools. Output: `builds/staged/Watchman_2.0.1_x64-setup.exe`. This is **unsigned** until the staging workflow signs it with the existing updater key. The C++ binary embeds its logo, icon, fonts, version and CompanyName. The native signature verifier is installed beside it. Tauri's CLI runs only on the packaging host to reuse the v1 NSIS contract; it does not build the app or install WebView2.

`src/` and `src-tauri/` retain the v1 reference implementation. Do not run the v1 build command to produce v2 artifacts.

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
cmake -S native -B builds/native -A x64
cmake --build builds/native --config Debug
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
