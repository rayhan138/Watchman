# Watchman SQLite History Migration Plan

## Goal

Move Watchman's traffic history storage from a growing JSON file to a local SQLite database so daily history is more stable, safer to query, and easier to extend later.

## Why We Are Doing This

Watchman currently stores traffic history in:

```text
%APPDATA%\Watchman\history.json
```

JSON is simple, but it becomes less ideal when the app needs long-term history, future per-app history, CSV exports, comparisons, and safer recovery. SQLite gives Watchman a real local database while keeping the same local-first promise.

## New Storage

The new database file will be:

```text
%APPDATA%\Watchman\watchman.db
```

This is still fully local. There is no server, cloud account, or external database.

## First Table

For this migration we only move daily traffic history:

```sql
CREATE TABLE IF NOT EXISTS daily_traffic (
  date TEXT PRIMARY KEY,
  upload_bytes INTEGER NOT NULL DEFAULT 0,
  download_bytes INTEGER NOT NULL DEFAULT 0,
  updated_at TEXT NOT NULL
);
```

## Migration Behavior

On startup:

1. Watchman opens or creates `watchman.db`.
2. Watchman creates the `daily_traffic` table if it does not exist.
3. If `history.json` exists, Watchman imports its records into SQLite.
4. The old JSON file is kept as a backup. It is not deleted.
5. After startup, new history is saved to SQLite.

## Runtime Behavior

During normal use:

1. Metrics are still collected every second.
2. The in-memory history map is updated immediately so the UI stays responsive.
3. Every 30 seconds, pending history is written to SQLite.
4. Reads for Data tab, usage totals, comparisons, and CSV export continue to use the same `HistoryState` API, so frontend code should not need changes.

## Safety Plan

If SQLite cannot be opened or read:

1. Watchman backs up the broken database with a timestamped `.bak` filename.
2. Watchman creates a clean new database.
3. Watchman tries to import from `history.json` or `history.json.bak` if available.
4. The app should keep running instead of black-screening.

## Files To Change

```text
src-tauri/Cargo.toml
src-tauri/Cargo.lock
src-tauri/src/commands/history.rs
```

No taskbar embedding files should be changed.

## Tests

We will add Rust tests for:

1. Creating a SQLite history database.
2. Saving multiple days and reading them back.
3. Preserving older dates during save.
4. Importing old `history.json` records into SQLite.
5. Returning daily, weekly, monthly, and yearly aggregates from SQLite-loaded data.

## Release Target

This should become Watchman `v1.0.4` after local testing and review.
