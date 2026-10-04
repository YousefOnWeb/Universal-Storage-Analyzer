**Phase 4: Design**

**Project Name:** Universal Storage Analyzer (FOSS)

**Document Version:** 4.0

**Phase:** Phase 4 — System Design & Contracts

---

## 1. UI/UX Design & Visual Layout

The UI/UX design focuses on high data density, real-time feedback, and native OS integration.

* **Main Application Layout:** The main window provides a clear, highly functional interface split into a file system tree on the left and a detailed analysis view on the right. Users can browse continuously updating results while a background scan is being performed.


* **Theming & Dark Mode:** The application features a comprehensive Dark Mode theme for the entire user interface. This can be explicitly toggled on and off by the user or automatically linked to the native Windows, macOS, or Linux system theme configurations.


* **Details Tabular View:** The Details view exposes deep metrics, including folder size, wasted disk space, last access dates, NTFS compression rates, and file owners. This pane supports custom column sorting and dynamic addition of new columns via a context menu.


* **Visual Charting Interfaces:**
* **Treemap Chart:** A hierarchical 2D and 3D rendering that visualizes the size of all subfolders (including deeply nested children) for a selected directory branch.


* **Bar Chart:** Offers a fast visual overview of disk space usage, complete with visualization options in the toolbar and the ability to print or copy to the clipboard.


* **Pie Chart:** An interactive, browsable circular chart detailing the relative storage portions of each subfolder within the currently selected directory.




* **Targeted Analytics Panes:**
* **Extensions View:** Breaks down disk usage by file types, allowing users to filter the left-hand file tree by specific extensions to easily locate dense clusters of specific file types.


* **Users View:** Groups and filters occupied disk space by user accounts to identify which owners are consuming the most capacity.


* **Top Files View:** Provides a direct overview pane listing the absolute largest files discovered during the most recent scan.




* **Options & Configuration:** The Options dialog enables deep customization of the application, such as configuring custom time intervals (e.g., creation date, last change, last access) for the "Age of Files" statistics, as well as modifying export properties and startup behavior.



---

## 2. Core Data Models (Sprint 1 Focus)

To support the requirement of a rapid, low-memory local directory scan, the foundational data model relies on a highly compressed, in-memory N-Ary tree.

### 2.1 File System Node (`FSNode`)

This C++ struct represents a single file or directory. It is aggressively packed to minimize RAM usage when mapping millions of items.

```cpp
struct FSNode {
    uint32_t id;                  // Unique internal identifier
    uint32_t parent_id;           // ID of the parent directory
    std::string name;             // File or folder name (UTF-8)
    uint64_t logical_size;        // Actual byte size of the file
    uint64_t allocated_size;      // Physical space consumed on disk (cluster aligned)
    uint32_t file_count;          // Number of child files (if directory)
    uint32_t folder_count;        // Number of child folders (if directory)
    uint64_t last_modified_ts;    // Unix timestamp for modification
    bool is_directory : 1;        // Bit-flag: 1 if directory, 0 if file
    bool is_compressed : 1;       // Bit-flag: NTFS/OS-level compression status
    std::vector<uint32_t> children; // IDs of child nodes (empty for files)
};

```

---

## 3. API and Integration Contracts

To fulfill the "Live Dual-Pane Visualization" user story, the architecture requires a strict asynchronous contract between the multithreaded C++ scanning backend and the Qt frontend.

### 3.1 Scanner Core API (`IScannerService`)

This interface dictates how the UI issues commands to the scanning engine.

```typescript
// Contract definition (Pseudo-code)
interface IScannerService {
    // Initializes a scan on a specific path. Returns a unique ScanTaskID.
    StartScan(targetPath: string): string; 
    
    // Pauses or resumes an active scan
    ToggleScanState(taskId: string, pause: boolean): void;
    
    // Gracefully terminates worker threads
    CancelScan(taskId: string): void; 
    
    // Fetches the aggregated metrics for a specific node to populate the Details view
    GetNodeMetrics(nodeId: uint32): FSNodeMetrics;
}

```

### 3.2 Asynchronous Event Stream (Backend -> UI)

Instead of the UI polling for data, the engine pushes updates via a thread-safe event bus (using Qt Signals/Slots).

* **`Event: NodeDiscovered`**
* **Payload:** `{ parentId: uint32, newNode: FSNode }`
* **Behavior:** Appends a new item to the UI's virtualized data model. Emitted in batches (e.g., every 500 items) to prevent UI thread starvation.


* **`Event: BranchMetricsUpdated`**
* **Payload:** `{ nodeId: uint32, newLogicalSize: uint64, newAllocatedSize: uint64 }`
* **Behavior:** Updates the size rendering in the File System Tree and Details panes dynamically while the scan is running.



---

## 4. Local Database Schema & Migration Scripts

While the core tree is in-memory for speed, a local SQLite database handles persistent settings, indexing historical scans, and scheduled task configurations.

### 4.1 Migration Script: `V1__Initial_Schema.sql`

This script executes on the first application launch to create the initial persistent data layer.

```sql
-- Migration: V1__Initial_Schema
-- Purpose: Establish settings store and snapshot indexing structures.

BEGIN TRANSACTION;

-- Table: AppSettings
-- Stores UI states, theming preferences, and custom file age intervals.
CREATE TABLE AppSettings (
    key_name VARCHAR(100) PRIMARY KEY,
    value_data TEXT NOT NULL,
    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- Seed initial settings
INSERT INTO AppSettings (key_name, value_data) VALUES ('theme_mode', 'system');
INSERT INTO AppSettings (key_name, value_data) VALUES ('age_intervals_days', '1,7,30,180,365');

-- Table: ScanSnapshots
-- Stores metadata for saved index files used in "Storage Growth Tracking".
CREATE TABLE ScanSnapshots (
    snapshot_id INTEGER PRIMARY KEY AUTOINCREMENT,
    target_path VARCHAR(1024) NOT NULL,
    total_logical_bytes BIGINT NOT NULL,
    total_allocated_bytes BIGINT NOT NULL,
    scan_date DATETIME DEFAULT CURRENT_TIMESTAMP,
    index_file_path VARCHAR(1024) NOT NULL -- Path to the serialized binary index on disk
);

-- Table: ScheduledTasks
-- Stores metadata for automated scans that interface with OS task schedulers.
CREATE TABLE ScheduledTasks (
    task_id INTEGER PRIMARY KEY AUTOINCREMENT,
    target_path VARCHAR(1024) NOT NULL,
    export_format VARCHAR(10) NOT NULL, -- e.g., 'PDF', 'CSV', 'XML'
    cron_schedule VARCHAR(50) NOT NULL,
    is_active BOOLEAN DEFAULT 1
);

COMMIT;

```