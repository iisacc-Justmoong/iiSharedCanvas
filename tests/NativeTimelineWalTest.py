"""Check Qt-free timeline export against a live, committed SQLite WAL."""

import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile


def run(*arguments):
    result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
    if result.returncode:
        raise AssertionError(f"{arguments[0]} failed: {result.stderr.decode('utf-8', errors='replace')}")


def digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


def main():
    cli, driver, work = sys.argv[1:]
    Path(work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="timeline-wal-", dir=work) as directory:
        root = Path(directory)
        source, updated = root / "원본.iisc", root / "updated.iisc"
        run(driver, "--create-working", str(source))
        run(driver, "--create-working-alt", str(updated))
        connection = sqlite3.connect(source, timeout=30)
        try:
            assert connection.execute("PRAGMA journal_mode=WAL").fetchone()[0].lower() == "wal"
            connection.execute("PRAGMA wal_autocheckpoint=0")
            connection.execute("ATTACH DATABASE ? AS updated", (str(updated),))
            connection.execute("BEGIN IMMEDIATE")
            connection.execute("UPDATE main.canvas_records AS target SET "
                               "data=(SELECT data FROM updated.canvas_records WHERE kind=target.kind AND id=target.id), "
                               "digest=(SELECT digest FROM updated.canvas_records WHERE kind=target.kind AND id=target.id)")
            connection.execute("UPDATE main.canvas_state SET revision=revision+1")
            connection.commit()
            wal = Path(str(source) + "-wal")
            assert wal.is_file() and wal.stat().st_size > 0
            original_main, original_wal = digest(source), digest(wal)
            for name, active_writer in (("committed", False), ("active-writer", True)):
                if active_writer:
                    connection.execute("BEGIN EXCLUSIVE")
                output = root / name
                run(cli, str(source), str(output))
                manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
                assert manifest["tracks"][0]["name"] == "WAL 변경", "export omitted committed WAL records"
                assert digest(source) == original_main and digest(wal) == original_wal, "export modified source or WAL"
                if active_writer:
                    connection.rollback()
        finally:
            connection.close()


if __name__ == "__main__":
    main()
