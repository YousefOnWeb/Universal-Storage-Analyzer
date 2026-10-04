-- Migration: V1__Initial_Schema
-- Purpose: Establish settings store and snapshot indexing structures.

BEGIN TRANSACTION;

-- Table: AppSettings
-- Stores UI states, theming preferences, and custom file age intervals.
CREATE TABLE IF NOT EXISTS AppSettings (
    key_name VARCHAR(100) PRIMARY KEY,
    value_data TEXT NOT NULL,
    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- Seed initial settings
INSERT OR IGNORE INTO AppSettings (key_name, value_data) VALUES ('theme_mode', 'system');
INSERT OR IGNORE INTO AppSettings (key_name, value_data) VALUES ('age_intervals_days', '1,7,30,180,365');

-- Table: ScanSnapshots
-- Stores metadata for saved index files used in "Storage Growth Tracking".
CREATE TABLE IF NOT EXISTS ScanSnapshots (
    snapshot_id INTEGER PRIMARY KEY AUTOINCREMENT,
    target_path VARCHAR(1024) NOT NULL,
    total_logical_bytes BIGINT NOT NULL,
    total_allocated_bytes BIGINT NOT NULL,
    scan_date DATETIME DEFAULT CURRENT_TIMESTAMP,
    index_file_path VARCHAR(1024) NOT NULL
);

-- Table: ScheduledTasks
-- Stores metadata for automated scans that interface with OS task schedulers.
CREATE TABLE IF NOT EXISTS ScheduledTasks (
    task_id INTEGER PRIMARY KEY AUTOINCREMENT,
    target_path VARCHAR(1024) NOT NULL,
    export_format VARCHAR(10) NOT NULL,
    cron_schedule VARCHAR(50) NOT NULL,
    is_active BOOLEAN DEFAULT 1
);

COMMIT;
