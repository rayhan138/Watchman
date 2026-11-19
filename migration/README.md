# 2.0 migration staging — incomplete, do not release

The native source of record is `rayhan138/watchman-c-` at
`3b04d7d9446c6efa2a11d6b35aea019e5f7685a0`. Read access is required to proceed.
Do not bump the Tauri project's version as a workaround. The repository's
existing tag-triggered/manual v1 release job **publishes**; do not dispatch it
or push a tag to stage this migration.

## Baseline and artifact gate

* The updater configs under `src-tauri/tauri.conf.json` in tags v1.0.1–v1.0.5
  contain the same full minisign public key. The v1.0.5 tag points to
  `2e490a181ce7eee09a38a13c65c375489b47a01c`; its config blob is
  `6f5f4b542b9e5f5681c6b7fe740b5c4f35bd2536`. That config specifies a
  per-machine NSIS installation and passive updater mode.
* The migration baseline PR job uses a fresh Windows runner. It downloads
  assets from **that tag**, checks the x64 manifest against the detached
  signature, verifies the actual installer with the pinned v1 key, checks
  rejection of a one-byte mutation, installs silently, and logs the installer
  hash and uninstall entry. This does **not** test an upgrade to v2.
* `New-StagedFeed.ps1 -ArtifactDirectory <staging-directory>` is a local-only
  packaging gate for CI after building and signing: it requires the final
  x64 installer and its Tauri-format `.sig`. The generated `latest.json`
  names the intended immutable v2 asset URL. It creates no tag, release or
  network resource. A staging server must use a separate test manifest/URL.
  The offline Rust verifier is tooling, not part of the installed app.

## Required before approval (not demonstrated by the baseline job)

1. Import the pinned native tree with resources/fonts; inspect the resulting
   executable, dependencies and native settings/history formats. Document a
   field-by-field mapping based on those formats; do not guess native keys.
2. Exercise the replacement NSIS on a running, installed v1.0.5, including
   passive updater arguments, non-default installation path, shutdown, one
   non-elevated restart, a single uninstall entry, shortcuts and per-user
   startup on/off. Test elevation with a different administrator account.
3. Back up each real user's `%APPDATA%\Watchman` before first native writes,
   **as that user**, not the elevating administrator. Preserve `settings.json`,
   `watchman.db` (and sidecars), `history.json`, backups and unknown files.
   Quiesce all writers before copying; fail without changing originals on
   error. Validate SQLite integrity and per-date byte totals on copies.
   Exercise JSON-only, mixed, missing, corrupt and repeated migration; test
   preservation of unknown settings and avoid counting overlapping days twice.
4. Independently exercise installed v1-to-v2 and native-to-native update paths
   on disposable Windows using controlled HTTPS staging (no production feed
   changes). Require user consent, a newer version and signature verification
   of the exact executable launched. Reject corrupt/unsigned payloads, wrong
   platform, replay/downgrade, untrusted redirects and failed downloads. Do not
   ship a trust-key or feed override for testing.
5. Use existing signing secrets only in approved CI, never PR jobs. Retain
   installer, signature, manifest, hashes and test logs as private staging
   artifacts or in appropriately access-controlled storage, not a release.

## Recovery rehearsal

Snapshot the disposable machine before upgrading. Record v1 paths, startup
state and uninstall registration; save the verified v1.0.5 installer. If the
candidate fails, stop Watchman, retain a separate copy of post-upgrade data,
remove the candidate without deleting user data, reinstall the verified v1
installer, then restore the complete quiescent pre-upgrade data directory for
each user (do not mix SQLite files and sidecars from different snapshots).
Verify history totals, settings, startup state and one working shortcut before
resuming normal use. Revert the VM snapshot if registration or data cannot be
restored. This is a recovery **plan**, not evidence of a successful rollback.
