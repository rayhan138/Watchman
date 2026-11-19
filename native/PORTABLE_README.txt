Watchman Native C++ - Portable Test Build

Requirements
- Windows 10 or Windows 11, 64-bit.
- No Visual C++ Redistributable, WebView, .NET, Rust, or other runtime install is required.

Run
Double-click Watchman.exe in the delivery folder. No installer is needed.
If using an optional archive, extract it before launching.

First-time welcome
- A three-screen quick tour is included in this same EXE: widgets/floating monitor,
  tray controls, and running as administrator for each app's upload/download bandwidth.
- The first screen shows realistic widget previews with labelled sample numbers.
- Use Next / Back / Skip / Get started. Esc skips; Tab and Enter work too.
- Skip and Get started remember your choice for this Windows user. Replay it in
  Settings > Quick tour (top of Settings) or press F1 in the main window.
- Quiet Windows startup does not pop up the tour. It waits until you open Watchman.
- The tour does not change your settings, start a speed test, or request administrator access.

Notes
- Inter, Nunito Sans ExtraBold and JetBrains Mono are inside the EXE and used
  directly from its resources. No font installation or separate fonts folder is needed.
- The floating monitor shows upload and download only, with white text by default.
  CPU/memory readouts and options are removed. Drag when unlocked; right-click for options.
  Saved custom colors are respected; Floating settings > Defaults > Save Changes restores white.
- Taskbar CPU/MEM labels have a wider, DPI-scaled gap before their percentage values.
- X / Alt+F4 hides Watchman to the system tray; monitoring and enabled widgets keep running.
  Double-click the tray icon or choose Open Watchman to restore it. Tray > Exit quits.
  This behavior does not depend on the startup toggle. If the tray is unavailable,
  the window stays open with a warning rather than becoming inaccessible.
- Tray > Restart Watchman (above Exit) relaunches the same EXE with the same
  normal/admin privileges after orderly shutdown. Hidden/minimized restarts stay
  quiet; visible main windows reopen. Unsaved settings drafts are not applied.
  If launching fails, the current instance stays running and shows an error.
  The replacement waits at most 15 seconds for shutdown; blocked I/O can time out.
- Hidden/minimized main windows release their own drawing resources; animations
  stop when hidden/minimized/inactive. Monitoring and enabled widgets continue.
  Inactive-tab snapshots are not copied unnecessarily. No measured MB saving is
  claimed; the reported 20-30 MB footprint is already modest.
- Settings use the current user's Registry. History uses APPDATA\Watchman\watchman.db.
  This is not an isolated profile; back up important history before testing.
- Startup defaults on for new settings; an existing saved off choice is respected.
  Keep the EXE in its final folder so Windows startup points at the intended path.
- Double-click the floating monitor or taskbar widget to open Data > Last 7 Days.
  Entering/reselecting Data resets to Last 7 Days, even after another range was selected.
- The title-bar sun/moon button switches directly between Light and Dark on each
  press, including rapid clicks. Settings > General > Auto follows Windows again
  after Save Changes. Warning setup/explanation text wraps within scrolling cards.
- CPU utilization uses busy processor time, not boost-scaled utility. Small timing
  differences from Task Manager are normal; warm-up/missing readings show --.
- GPU temperature uses the Windows driver when supported. For CPU/disk/mainboard,
  run the optional LibreHardwareMonitor backend with its web server bound ONLY to
  127.0.0.1:8085 (not LAN/all interfaces). Keep it running. No helper or driver is
  bundled or installed by Watchman. See Dashboard > System Information and
  Settings > Warnings for readings, sources, thresholds and setup notes.
- Traffic, memory and temperature alerts use Windows notifications. Missing/stale
  sensors do not notify; temperature/memory rules have recovery and cooldown.
- M-Lab tests use substantial data and publicly publish IP/time/results; consent is required.
- This test build is not code-signed. Windows SmartScreen may show an "unknown publisher" prompt.
  This is not a missing dependency or proof of safety. Antivirus acceptance is not guaranteed.
  Keep protections enabled; retain any exact detection name for the vendor's review process.
