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

Update notifications
- Newer published stable releases in rayhan138/Watchman are checked in the background,
  starting 15 seconds after UI readiness, then daily while the app stays running.
- Three notices: native Windows notification with versions; a dismissible full-window
  prompt when you open the app; tiny Update available / Download now footer link
  between Connected and Uptime. Quiet startup never forces the dashboard open.
- Cross / Esc / Not now dismiss the prompt for that version; the footer link remains.
  Tab / Shift+Tab and Enter / Space work. Windows alerts in Warnings controls OS notices.
- Tools > Check for updates performs a real check. Offline/errors are not called current.
- Download now opens the official release page in your browser. No automatic download,
  installation, elevation, restart, history replacement or startup change occurs.
- GitHub receives the normal request/IP and Watchman version, not local history/app data.
  Builds without the checker cannot gain it remotely; publishing only a commit or
  test-repository artifact does not announce an update. Windows may suppress OS notices.

Notes
- Inter, Nunito Sans ExtraBold and JetBrains Mono are inside the EXE and used
  directly from its resources. No font installation or separate fonts folder is needed.
- The floating monitor shows upload/download plus optional CPU and memory percentages.
  Settings > Floating has separate CPU/memory switches and color controls. Right-click
  offers the same visibility shortcuts. Hide both for the original compact strip.
  It reuses existing live readings; stale/missing CPU or memory shows --, not zero.
  Drag when unlocked. Floating > Text style offers Light (default), Regular and Bold,
  plus 11-16 px size controls (default 12 px). Text is thin/white by default, with no
  white/black outline or shadow on custom colors or transparent backgrounds.
  The preview shows the draft weight/size/colors. Save Changes applies; Undo or
  closing Settings discards edits. Right-click Text weight / Text size saves immediately.
  Existing colors/sizes are kept. Defaults > Save Changes restores Light, white,
  12 px and both readings without resetting unrelated monitoring/startup preferences.
- Tools CSV exports have five clean columns, binary MB/GB numeric values, friendly
  dates and a readable Summary. Multiple saved rows have a blank separator before
  TOTAL/Average; single-row reports do not. UTF-8 BOM and CRLF support Excel opening.
  Select the first four columns above the blank row for charts. No raw bytes are exported.
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
- Settings use the current user's Registry. If APPDATA\Watchman.Native exists,
  its existing watchman.db is used (compatible with installed native v2 Watchman).
  Otherwise the standalone/legacy APPDATA\Watchman\watchman.db path is retained.
  Both folders and original-v1/JSON backups are kept; separate histories are NOT merged.
  A missing, empty or unreadable native database is reported, never replaced by an
  empty database or silently redirected to legacy. Tray > Monitoring status shows
  the selected profile and any history read/save problem.
  This is not an isolated profile. Exit both versions through the tray and copy
  BOTH complete data folders somewhere safe before testing. Do not mix sidecars.
- The taskbar gets a lightweight GDI first frame before optional GPU setup.
  Dashboard fonts/graphics are deferred until you open it; GPU counter discovery
  runs separately from network sampling. Settled windows do not run a 60Hz timer.
- Startup defaults on for new settings; an existing saved off choice is respected.
  This candidate uses a Watchman-only, current-user Task Scheduler sign-in task:
  "Watchman Sign-in <your user SID>" in Task Scheduler Library, not Startup Apps.
  It requests normal privileges, no stored password and no added trigger delay.
  General > Run on Windows startup > Save Changes manages it in the background.
  The old recognized Run entry is removed only after the task is verified.
  Off removes both methods. Tray > Monitoring status reports completion/errors.
  Externally disabled startup is not silently re-enabled; save Off then On to
  enable it deliberately. Keep the EXE in its permanent folder; launch it once
  after moving it to update an enabled task. Disable startup before deleting it.
  No global startup-delay, Startup Apps approval or UAC setting is changed.
  Earlier appearance still needs testing on your PC; wWinMain-to-frame timing
  is not OS process creation, Windows sign-in timing or physical-pixel proof.
  To revert: save startup Off here, confirm removal in Monitoring status, Exit,
  then open the previous build and deliberately turn its startup On again.
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
