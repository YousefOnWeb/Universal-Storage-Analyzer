**Phase 2: Architecture & Technical Specification**

**Project Name:** Universal Storage Analyzer (FOSS)

**Document Version:** 2.0

**Phase:** Phase 2 — Technical Architecture & System Design

## 1. System Architecture & Technology Stack

The application relies on a decoupled, high-performance architecture separating the multithreaded scanning engine from the graphical user interface.

* **Core Engine:** Implemented in C++ (or Rust) to enable zero-cost abstractions, efficient raw memory management, and direct operating system API access across disparate platforms.
* **UI Framework:** Developed using the Qt framework to provide native, cross-platform UI rendering across Windows, macOS, and Linux while maintaining high frame rates for 2D/3D charting.
* **Local Data Store:** Utilizes local SQLite databases for saving localized application settings, caching deep directory scans, and storing historical index files for differential state comparisons.

## 2. High-Performance Traversal Engine

The core of the analyzer is built to process massive file hierarchies rapidly without locking the user interface.

* **Direct File System Access:** For local Windows NTFS drives, the engine bypasses standard high-level Windows APIs to read directly from the Master File Table (MFT), achieving maximum sequential read speeds. Native POSIX `dirent` APIs handle macOS and Linux local filesystems.
* **Adaptive Multithreading:** The traversal engine dynamically spawns parallel worker threads—scaling up to 32 concurrent threads depending on the target storage limits, network latency, and available CPU cores.
* **Virtual File System (VFS) Layer:** A unified abstraction layer maps various remote protocols to a standardized internal file node, supporting:
* **Cloud & Network:** Native integrations for Amazon S3, Azure Blob Storage, WebDAV, SharePoint Online, and local SMB/NFS network shares.
* **Remote Unix/Linux:** Secure Shell (SSH) traversal enabling the scanning of remote Linux servers without requiring local agent installation.
* **Mobile Storage:** Media Transfer Protocol (MTP) integration for scanning smartphones and mobile devices directly from the desktop.
* **Mailboxes & Images:** Support for scanning Outlook mailboxes (via MAPI/EWS) and natively mounting VHDX drive images.



## 3. Memory Management & Data Structures

Given the necessity to map millions of files simultaneously, memory footprint efficiency is paramount.

* **Hierarchical Node Tree:** Files and directories are mapped to an n-ary tree in memory. Node structs are tightly packed using pointer compression and minimal byte footprints to store logical size, allocated physical space, timestamps, and owner IDs.
* **Asynchronous UI Virtualization:** The user interface heavily relies on virtualized data lists. Only on-screen data is rendered by the GPU, while background threads continuously populate the backing data model. This enables users to seamlessly browse and interact with the dynamically updating results list while the scan is actively running in the background.



## 4. Deduplication & Mass Operations Subsystem

The engine provides direct file manipulation capabilities alongside automated redundancy cleanup.

* **Cryptographic Deduplication Engine:** Duplicate file scanning operates on byte-for-byte evaluations backed by hardware-accelerated MD5 or SHA256 checksum generation.
* **Hardlink Re-linking:** Discovered duplicates can be natively deduplicated by replacing redundant files with system-level hardlinks (NTFS hardlinks on Windows, POSIX hardlinks on Unix). This instantly reclaims physical storage without breaking dependent application paths or user access.
* **Batch Remediation:** A queued mass-operations module executes archiving, moving, bulk deletion, and regular expression (regex)-based mass renaming for tens of thousands of items simultaneously.

## 5. Visual Rendering & UI Architecture

The presentation layer strictly implements the Model-View-Controller (MVC) pattern, surfacing analytics through a highly adaptable split-pane interface.

* **Dual-Pane Navigation:** The main window splits into an interactive File System Tree on the left and a dynamic multi-tab interface on the right for granular detail inspection and charting. Full Unicode support is provided for diverse file and directory names.


* **Hardware-Accelerated Chart Engine:**
* **Treemaps:** A squarified treemap algorithm renders hierarchical tile charts representing nested folder sizes across entire branches. It supports interactive 2D and 3D rendering modes, visually scaling and coloring tiles based on storage consumption.


* **Bar & Pie Charts:** Interactive bar charts offer quick background space visualization, while browsable pie charts display relative capacity portions of subfolders alongside available free space.




* **Categorized Analytics Panes:**
* **Details View:** Sortable tables exposing logical size, allocated space, file counts, last access dates, owner data, and filesystem-level NTFS compression rates.


* **Extensions & Users:** Scan data is pivoted to display capacity distribution by file type extensions (e.g., .dll, .exe, .tmp, .bak) and Active Directory or local user ownership.


* **Top Files:** A dedicated analytics pane actively filters and lists the largest single files discovered across the entire target volume.




* **Configurable View Parameters:** Users can customize display columns via context menus, configure export properties, and define custom chronological intervals (days, weeks, months, years) for "age of files" distribution tracking.


* **Dynamic Theming:** The UI engine incorporates a native Dark Mode, which can be toggled manually via user settings or automatically linked to match the host operating system's global theme configurations.



## 6. Exporting & Storage Growth Tracking

Data portability and historical comparison are managed through decoupled serialization protocols.

* **Multi-Format Export Engine:** Dedicated serializers allow users to export scan hierarchies, tables, and visual charts to a variety of formats including PDF, HTML, XML, Microsoft Excel, CSV, or direct email injection.
* **Differential Storage Tracking:** Complete directory hierarchies can be serialized into lightweight index files. The core engine can load an older index file and execute a real-time delta comparison against a current live scan, automatically highlighting capacity differences, file additions, and specific folder growth trajectories.
* **Snapshot Integration:** On supported operating systems, the engine hooks into native volume snapshots (e.g., Windows Shadow Copies) to extract and analyze historical block data without manual index creation.

## 7. OS Integration & Task Automation

The application deeply integrates with host operating systems to function as a seamless native utility.

* **File Manager Bindings:** Shell extensions allow the application to launch directly from context menus within Windows Explorer, macOS Finder, and Linux file managers, fully supporting bidirectional drag-and-drop operations.
* **Cross-Platform Task Scheduler:** A dedicated scheduling dialog allows users to compile custom scan profiles (defining target paths, export formats, and filter rules) and injects them directly into native OS job schedulers (Windows Scheduled Tasks, Linux cron, macOS launchd) for unattended background execution.