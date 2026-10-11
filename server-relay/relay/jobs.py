"""Current task ledger. No old-database import or historical rebinding tables."""
RUNTIME_SCHEMA_VERSION = 2
STORAGE_MANIFEST = {"runtime_schema_read": [2], "runtime_schema_write": 2,
                    "identity_schema_read": [2], "identity_schema_write": 2,
                    "minimum_runtime_reader": 2}


def initialize_schema(store, owner):
    with store.connect() as db:
        db.executescript("""
            CREATE TABLE IF NOT EXISTS relay_runtime_meta (
                singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                schema_version INTEGER NOT NULL, owner_generation TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS relay_jobs (
                task_id TEXT PRIMARY KEY, device_id TEXT NOT NULL, recording_id INTEGER NOT NULL,
                source TEXT NOT NULL, revision TEXT, target_id TEXT, phase TEXT NOT NULL,
                owner_generation TEXT, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
                claimed_hash TEXT, audio_bytes INTEGER, wav_name TEXT, pin_id TEXT,
                UNIQUE(device_id,recording_id)
            );
        """)
        db.execute("INSERT OR IGNORE INTO relay_runtime_meta VALUES(1,?,?)", (RUNTIME_SCHEMA_VERSION, owner))
        row = db.execute("SELECT schema_version FROM relay_runtime_meta WHERE singleton=1").fetchone()
        if row[0] != RUNTIME_SCHEMA_VERSION:
            raise RuntimeError("unsupported runtime schema")
        db.execute("UPDATE relay_runtime_meta SET owner_generation=? WHERE singleton=1", (owner,))
