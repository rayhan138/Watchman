"""Exercise the installed C++ first-launch migration on disposable profile directories."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile


def migrate(root, success=True):
    result = subprocess.run([sys.argv[1], '--migrate-only'], timeout=30,
                            env={**os.environ, 'APPDATA': str(root)})
    assert (result.returncode == 0) == success, (root, result.returncode)


def hashes(directory):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir() if p.is_file()}


def profile(root):
    old = root / 'Watchman'
    old.mkdir(parents=True)
    return old, root / 'Watchman.Native'


def json_history(old):
    records = {'records': {'2025-01-01': {'upload': 999, 'download': 888},
                           '2025-01-02': {'upload': 777, 'download': 666}}}
    for name in ('history.json', 'history.json.bak'):
        (old / name).write_text(json.dumps(records), encoding='utf8')


def old_native(root):
    old, active = profile(root)
    json_history(old)
    # Model an old C++ schema, including a committed transaction still in WAL.
    # Snapshot a quiescent fixture into the test profile; close the generator
    # only after copying, so SQLite doesn't checkpoint away the test case.
    seed = root / 'seed.db'
    conn = sqlite3.connect(seed)
    conn.execute('pragma journal_mode=wal')
    conn.executescript("""
        CREATE TABLE daily_traffic(date TEXT PRIMARY KEY, download_bytes INTEGER, upload_bytes INTEGER);
        CREATE TABLE app_traffic(date TEXT, app_name TEXT, download_bytes INTEGER, upload_bytes INTEGER,
                                 PRIMARY KEY(date,app_name));
        INSERT INTO daily_traffic VALUES ('2025-01-01', 42, 17);
        INSERT INTO app_traffic VALUES ('2025-01-01', 'earlier.exe', 40, 16);
    """)
    for suffix in ('', '-wal', '-shm'):
        shutil.copyfile(str(seed) + suffix, old / ('watchman.db' + suffix))
    conn.close()
    before = hashes(old)
    migrate(root)
    assert hashes(old) == before == hashes(active / 'original-v1')
    db = active / 'watchman.db'
    with sqlite3.connect(db) as c:
        assert c.execute('pragma integrity_check').fetchone() == ('ok',)
        assert c.execute('select * from daily_traffic').fetchall() == [('2025-01-01', 42, 17)]
        assert c.execute('select * from app_traffic').fetchall() == [('2025-01-01', 'earlier.exe', 40, 16)]
        # This native-only row must survive installation over an already migrated profile.
        c.execute("insert into daily_traffic values ('2025-01-03', 13, 31)")
    c.close()
    native_before = hashes(active)
    migrate(root)
    assert hashes(active) == native_before and hashes(old) == before


def json_only(root):
    old, active = profile(root)
    json_history(old)
    before = hashes(old)
    migrate(root)
    with sqlite3.connect(active / 'watchman.db') as c:
        assert c.execute('select count(*) from daily_traffic').fetchone() == (0,)
    c.close()
    assert hashes(old) == before == hashes(active / 'original-v1')


def damaged(root, name, data):
    old, active = profile(root)
    json_history(old)
    (old / name).write_bytes(data)
    before = hashes(old)
    migrate(root, success=False)
    assert not active.exists() and hashes(old) == before


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        old_native(root / 'native-history')
        json_only(root / 'json-only')
        damaged(root / 'truncated', 'watchman.db', b'')
        damaged(root / 'missing-main', 'watchman.db-wal', b'orphaned')
    print('SQLite-only history policy passed on installed executable.')
