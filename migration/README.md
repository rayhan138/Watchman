# Watchman 2.0.0 migration candidate — staging only

Do not publish, tag, set Latest, change the production feed, or merge the draft PR as part of staging.

## Provenance and package

The supplied GitHub archive's root name and ZIP comment identify `rayhan138/watchman-c-@3b04d7d9446c6efa2a11d6b35aea019e5f7685a0`; the decoded ZIP SHA-256 is `bac1be3f7991de5785db66cc63569c0da123d075c1ab05bb57e85e10c6a11ab7`. Application code, SQLite amalgamation, RC, icon, logo, fonts/licenses, build inputs and tests are in `native/`. The import excludes agent metadata and the upstream release workflow. `third_party/nlohmann` is the MIT-licensed single header from v3.12.0 added for JSON parsing.

`Build-Native.ps1` builds/tests x64 MSVC Release and uses the locked Tauri CLI **bundle only** command on the prebuilt C++ executable. `src-tauri/native-package.json` reuses the old per-machine NSIS identity/install path, uninstall registration, shortcut behavior, running-process check, and `/P /UPDATE /R` restart-as-user contract. It skips WebView2 and replaces the v1 resource list. The post-install hook deletes known loose v1 probe files, not unknown user files. The installer does not remove a shared Windows WebView2 runtime. Per-user registration and migration happen when that user launches the new app, not by scanning profiles from an elevated installer. Existing v1 `--minimized` startup commands are understood and rewritten to the native form.

## Data contract

* Source: `%APPDATA%\Watchman`. New active history: `%APPDATA%\Watchman.Native\watchman.db`. The legacy tree is not reused for writes. On first launch, after acquiring the instance guard, the app stages a full copy under a uniquely named sibling directory (rejects reparse points and locked data). It copies the DB plus sidecars from that snapshot to a **working** DB and integrity-checks it. A completed staging directory is renamed into place. `original-v1` inside it is the backup, not a live DB. Allow space for the snapshot **and** working database. Other Tauri identifier-specific app/WebView directories are untouched.
* **SQLite is the only history source**, including daily/per-app tables from earlier C++ builds. Copy the DB and its recovery files as a set. The app's normal schema upgrades are additive. If no DB or sidecars exist, start with an empty SQLite history. `history.json` and `history.json.bak` are copied as opaque backup files: neither is parsed, merged, changed, nor used for recovery. A corrupt DB, zero-length DB, or orphaned sidecars stop migration rather than substituting an empty DB. Incomplete `Watchman.Migrating-*` directories can be inspected offline.
* A completed native profile stays active. The migration does not remove any existing records, including records that a previous build may already have imported. An unmarked conflicting destination is left alone, not reset.
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

`Test-LegacyInstall.ps1` checks genuine v1 assets and a tampered copy. `Test-Upgrade.ps1` starts installed v1, upgrades via NSIS, and checks path/registration, restarted process, native executable hash, startup and shortcut, retained SQLite and untouched JSON, fresh history for JSON-only profiles, idempotence and uninstall. Signed staging downloads the exact CI artifact on a separate Windows runner. `Test-Staging.ps1` serves a **copy** of the manifest with only its URL changed over localhost HTTPS; a separate build of the native update code (lower current version and loopback-only channel) checks accepted and rejected responses and launches the verified installer. No production DNS override, replacement signing trust, or production feed write. The stock v1 UI is not redirected to staging.

## Acceptance evidence

Use the [new signed staging artifact](https://github.com/rayhan138/Watchman/actions/runs/44613120194/artifacts/14573248074) built from `a8e6314e4ea27fe79270f4f7fc6be59e8674188a`. It supersedes the earlier 2.0.0 staging build, which imported history JSON. The Settings header shows **v2.0.0**; because both candidates have the same version, identify this build by its artifact ID and hash, not by the badge alone.

* [Build, CTest, CI signing and verification against released v1 trust](https://github.com/rayhan138/Watchman/actions/runs/44613120194/job/135874628219): passed. Archive SHA-256: `673744648717cb9a5da65cd84073785ca4aae4ce60a7149422e3ac1e74d400b8` (ZIP, not EXE). The archive contains the installer, `.sig`, staged `latest.json` and expected installed app hash.
* [Independent signed-byte acceptance on disposable Windows](https://github.com/rayhan138/Watchman/actions/runs/44613120194/job/135876263116): passed. The second runner upgraded installed v1.0.5 from a non-default path via controlled HTTPS, tested settings, startup, shortcuts, uninstall and rollback. Installed-executable fixtures checked daily/per-app SQLite from an earlier C++ schema with committed WAL data, valid conflicting JSON, fresh empty history with JSON only, untouched originals/backups, survival of an existing native-only row, and refusal to replace corrupt/truncated/orphaned history. The production feed was not redirected or modified.
* [Default-path v1.0.5 upgrade on Windows](https://github.com/rayhan138/Watchman/actions/runs/44613128680/job/135874648427): passed.

## Recovery and limitations

1. Before rollout, snapshot the VM or independently back up the user's data while Watchman is stopped. Record install path, Run state, native settings if any and daily totals; cache and verify the v1.0.5 installer/signature.
2. On failure, stop Watchman, retain both data trees (including SQLite `-wal`/`-shm`) and any interrupted staging copy. Do not delete user data in the uninstaller. Uninstall v2, then reinstall the verified v1 binary at the original path. v1 should find its untouched original directory. If restoration is necessary, restore the **whole** quiescent directory from `original-v1` or the external backup. Never mix sidecars across snapshots. v2-only traffic after migration is not automatically merged back.
3. Launch v1 as the real user; check totals, settings, shortcuts and per-user startup. Before retrying v2 after rollback, archive the completed native profile and any imported native registry values if a fresh migration is needed. Do not overwrite newer data without comparing both histories. Revert the VM snapshot if recovery is uncertain.
4. Hosted Windows checks do not substitute for manual UAC with a different admin, multiple logged-on users, interactive cancel/restart, unusual ACLs or roaming profiles, interrupted power/disk writes, and real Win10/11 desktops. Machine uninstall cannot clear another user's HKCU Run value. Look for stale Run entries on shared machines. The inherited NSIS v1 fallback can force-stop a running process; unsaved in-memory samples cannot be recovered from a disk backup. Rollback to v1 is explicit, not an automatic feed downgrade.
