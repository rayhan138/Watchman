"""Seed and verify an already-migrated native profile across an in-place update."""
from pathlib import Path
import sqlite3
import sys

FIXTURE_DATE = "2025-01-01"
FIXTURE_APP = "preserved-profile-fixture.exe"


def seed(database: Path) -> None:
    database.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(database) as connection:
        connection.executescript(
            """
            CREATE TABLE daily_traffic (
                date TEXT PRIMARY KEY,
                download_bytes INTEGER NOT NULL DEFAULT 0,
                upload_bytes INTEGER NOT NULL DEFAULT 0,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE app_traffic (
                date TEXT,
                app_name TEXT,
                download_bytes INTEGER NOT NULL DEFAULT 0,
                upload_bytes INTEGER NOT NULL DEFAULT 0,
                last_seen INTEGER DEFAULT 0,
                PRIMARY KEY (date, app_name)
            );
            """
        )
        connection.execute(
            "INSERT INTO daily_traffic VALUES (?, ?, ?, ?)",
            (FIXTURE_DATE, 707, 808, "hotfix-test-fixture"),
        )
        connection.execute(
            "INSERT INTO app_traffic VALUES (?, ?, ?, ?, ?)",
            (FIXTURE_DATE, FIXTURE_APP, 701, 801, 1735689600),
        )


def verify(database: Path) -> None:
    with sqlite3.connect(database) as connection:
        assert connection.execute("PRAGMA integrity_check").fetchone() == ("ok",)
        assert connection.execute(
            "SELECT download_bytes, upload_bytes, updated_at FROM daily_traffic WHERE date = ?",
            (FIXTURE_DATE,),
        ).fetchone() == (707, 808, "hotfix-test-fixture")
        assert connection.execute(
            "SELECT download_bytes, upload_bytes, last_seen FROM app_traffic WHERE date = ? AND app_name = ?",
            (FIXTURE_DATE, FIXTURE_APP),
        ).fetchone() == (701, 801, 1735689600)
        # This date appears only in the archived JSON fixture, never in SQLite.
        assert connection.execute(
            "SELECT COUNT(*) FROM daily_traffic WHERE date = '2025-01-02'"
        ).fetchone() == (0,)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in {"seed", "verify"}:
        raise SystemExit("usage: test_native_upgrade.py seed|verify PATH_TO_DB")
    action, database_arg = sys.argv[1:]
    if action == "seed":
        seed(Path(database_arg))
    else:
        verify(Path(database_arg))
    print(f"Native profile {action} check passed: {database_arg}")
