# ADR 0001 — Portable traversal for Sprint 1, MFT later

Date: 2026-10-04 · Status: **superseded by ADR 0002**

> The fast path landed earlier than planned: direct volume reading is
> implemented and selected automatically (see ADR 0002). The reasoning
> below is kept for history; "Milestone 2" now refers to remote/cloud
> protocols instead.

Context: the discovery docs propose MFT-direct reads on Windows. That is
high-risk for a first vertical slice (admin rights, NTFS internals, testing).

Decision: Sprint 1 uses portable `std::filesystem` traversal in a worker
thread with batched queued delivery. MFT/VFS fast paths land behind the
`IScannerService` interface in Milestone 2 without UI changes.

Consequences: slightly lower NVMe peak throughput now; far higher
correctness and cross-platform confidence for the foundation.
