# Watchman 2.0.0 migration candidate — staging only

Do not publish, tag, set Latest, change the production feed, or merge the draft PR as part of staging.

## Provenance and package

The supplied GitHub archive's root name and ZIP comment identify `rayhan138/watchman-c-@3b04d7d9446c6efa2a11d6b35aea019e5f7685a0`; the decoded ZIP SHA-256 is `bac1be3f7991de5785db66cc63569c0da123d075c1ab05bb57e85e10c6a11ab7`. Application code, SQLite amalgamation, RC, icon, logo, fonts/licenses, build inputs and tests are in `native/`. The import excludes agent metadata and the upstream release workflow. `third_party/nlohmann` is the MIT-licensed single header from v3.12.0 added for JSON parsing.

`Build-Native.ps1` builds/tests x64 MSVC Release and uses the locked Tauri CLI **bundle only** command on the prebuilt C++ executable. `src-tauri/native-package.json` reuses the old per-machine NSIS identity/install path, uninstall registration, shortcut behavior, running-process check, and `/P /UPDATE /R` restart-as-user contract. It skips WebView2 and replaces the v1 resource list. The post-install hook deletes known loose v1 probe files, not unknown user files. The installer does not remove a shared Windows WebView2 runtime. Per-user registration and migration happen when that user launches the new app, not by scanning profiles from an elevated installer. Existing v1 `--minimized` startup commands are understood and rewritten to the native form.

## Data contract

* Source: `%APPDATA%\Watchman`. New active history: `%APPDATA%\Watchman.Native\watchman.db`. The legacy tree is not reused for writes. On first launch, after acquiring the instance guard, the app stages a full copy under a uniquely named sibling directory (rejects reparse points and locked data). It copies the DB plus sidecars from that snapshot to a **working** DB and integrity-checks it. A completed staging directory is renamed into place. `original-v1` inside it is the backup, not a live DB. Allow space for the snapshot **and** working database. Other Tauri identifier-specific app/WebView directories are untouched.
* `history.json.records[YYYY-MM-DD]` contains `upload`/`download` bytes. Missing/unparseable JSON can fall back to `history.json.bak`. A SQLite transaction takes the maximum **per direction per day**, not the sum of overlapping sources. Extra tables, app rows and unknown files are retained. A complete profile is not re-imported. Bad DB integrity, invalid history values/dates, both JSON sources unreadable, disk full, or a conflicting destination stop migration; no silent empty history. Incomplete `Watchman.Migrating-*` directories can be inspected offline.
* Native settings are written to `HKCU\Software\Watchman`, without replacing existing native values. Registry writes are individually idempotent, not one cross-file/registry transaction. If interrupted, retry before manually editing imported registry values.

| v1 settings.json | Native setting |
|---|---|
| `startOnBoot`, `unitModeBits`, `hideGauges` | Autostart, UnitModeBits, HideHardwareGauges |
| `theme` = dark/light/system | Theme = 0/1/2 |
| `dataLimit`, `dataLimitEnabled` | DataLimitBytes (zero when disabled) |
| `widgetSettings.mode` = full/network-only/system-only | WidgetMode = 0/1/2 |
| `notifications.enabled`, `highUsageWarnings` | Combined alerts toggle |
| `notifications.warningSettings` | Traffic toggle/threshold and MB/GB unit, memory toggle/percent, CPU/GPU/disk/mainboard temperature toggles and thresholds |
| `memoryWarningEnabled`, `memoryWarningThreshold` | Fallback when nested memory settings absent |

Not translated: focus theme; other widget styling, order and individual visibility controls; speed-test results and server selection; period start; active profiles; connection/data/slow-network notifications and sounds; network-health, monitoring intervals, troubleshooter and export presets; unknown fields. They remain in the original JSON and snapshot. Native defaults apply where there is no equivalent or a scalar is outside its supported range. Legacy browser-side state is not imported. The native app's optional temperature backend differs from v1's bundled PowerShell/DLL probe.

## Update trust and staging

The full public key is identical in tags v1.0.1–v1.0.5. v1.0.5 is `2e490a181ce7eee09a38a13c65c375489b47a01c` (updater config blob `6f5f4b542b9e5f5681c6b7fe740b5c4f35bd2536`). Native Tools → Check for updates uses WinHTTP/TLS, a fixed production feed, bounded downloads, restricted HTTPS redirects, a strictly newer numeric version, and the exact x64 tag/asset URL. It prompts before download/install. The native verifier checks the **bytes and signed filename** with the pinned key; PE product version must also match. The executable is locked against writing/deletion through launch. There is no production command-line feed/key override. Run the updater unelevated; NSIS requests elevation. Verification is updater trust, **not Authenticode**. SmartScreen may still prompt. A successfully handed-off installer can remain under the user's temp directory.

`release-windows.yml` on this branch is **manual, artifact-only** (`contents: read`); it no longer publishes on tags. Only its signing step receives the existing secrets, not a PR job. `New-StagedFeed.ps1` verifies before creating `latest.json` in a fresh staging directory; the final URL deliberately will not work until a separately approved release. Do not dispatch the *old main-branch workflow* to stage this candidate. Keep the three assets from the **same successful run**; never substitute a rebuilt EXE for a signed one.

`Test-LegacyInstall.ps1` checks genuine v1 assets and a tampered copy. `Test-Upgrade.ps1` starts installed v1, upgrades via NSIS, and checks path/registration, restarted process, native executable hash, startup and shortcut, retained SQLite and JSON, old backup-only recovery, idempotence and uninstall. Signed staging downloads the exact CI artifact on a separate Windows runner. `Test-Staging.ps1` serves a **copy** of the manifest with only its URL changed over localhost HTTPS; a separate build of the native update code (lower current version and loopback-only channel) checks accepted and rejected responses and launches the verified installer. No production DNS override, replacement signing trust, or production feed write. The stock v1 UI is not redirected to staging.

## Acceptance evidence — 2026-11-19 UTC

| Check | Evidence |
|---|---|
| MSVC build, native CTest, default-path v1.0.5 → v2, older data, uninstall and rollback | [Windows PR job](https://github.com/rayhan138/Watchman/actions/runs/44600073125/job/135828732886) |
| Sign and verify the actual 2.0.0 NSIS EXE and generate its v1-compatible feed | [Successful signing/packaging job](https://github.com/rayhan138/Watchman/actions/runs/44600054017/job/135828679604), source `66c5b4c49703e85774b481d0fda29854d1d0ebf8` |
| **Exact signed bytes**, independent Windows VM, custom protected install path, controlled HTTPS, no rebuild or signing | [Successful artifact retest](https://github.com/rayhan138/Watchman/actions/runs/44600678491/job/135830930957) |

[Download `watchman-2.0.0-staged`](https://github.com/rayhan138/Watchman/actions/runs/44600054017/artifacts/14567043658) (artifact retention until 2027-02-17 UTC). ZIP SHA-256 reported by GitHub: `10ba2e6bbb0b8801551035b6aa51fa1307fa8108f1a9767f05fb0c29e831150d`. This is the **artifact ZIP** digest, not the installer digest. The signing job summary lists individual file hashes. The archive includes `Watchman_2.0.0_x64-setup.exe`, its `.sig`, `latest.json`, and the expected installed app hash.

The independent test passed with v1 running. It checked shutdown/restart, one machine uninstall entry, shortcut target, startup enabled/disabled, installed app hash, backup hashes, SQLite integrity and daily byte counts, JSON backup-only recovery and repeated migration. A damaged DB stopped migration without changing the original. Uninstall retained data; reinstalling the verified v1 installer restored v1 registration and the original history totals. The protocol probe rejected an HTTP redirect, wrong platform, disallowed asset URL, invalid signature and changed installer bytes, and did not install an equal version. The signing run's original QA attempt was stopped after its user certificate-store setup stalled; the linked passing retest used the **same artifact**, not a replacement, and removed its temporary localhost machine-store certificate. Test code at `3610acc` differs from the signing commit only in the staging harness/workflow. No release was created.

## Recovery and limitations

1. Before rollout, snapshot the VM or independently back up the user's data while Watchman is stopped. Record install path, Run state, native settings if any and daily totals; cache and verify the v1.0.5 installer/signature.
2. On failure, stop Watchman, retain both data trees (including SQLite `-wal`/`-shm`) and any interrupted staging copy. Do not delete user data in the uninstaller. Uninstall v2, then reinstall the verified v1 binary at the original path. v1 should find its untouched original directory. If restoration is necessary, restore the **whole** quiescent directory from `original-v1` or the external backup. Never mix sidecars across snapshots. v2-only traffic after migration is not automatically merged back.
3. Launch v1 as the real user; check totals, settings, shortcuts and per-user startup. Before retrying v2 after rollback, archive the completed native profile and any imported native registry values if a fresh migration is needed. Do not overwrite newer data without comparing both histories. Revert the VM snapshot if recovery is uncertain.
4. Hosted Windows checks do not substitute for manual UAC with a different admin, multiple logged-on users, interactive cancel/restart, unusual ACLs or roaming profiles, interrupted power/disk writes, and real Win10/11 desktops. Machine uninstall cannot clear another user's HKCU Run value. Look for stale Run entries on shared machines. The inherited NSIS v1 fallback can force-stop a running process; unsaved in-memory samples cannot be recovered from a disk backup. Rollback to v1 is explicit, not an automatic feed downgrade.
