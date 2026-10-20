# Milestone follow-ups

- M5: exercise deletion of obsolete SSTables while a Windows mapping is still
  open. Verify that live readers retain their data and files are reclaimed once
  the owning versions and mappings are released.
- M5: reclaim unreferenced WALs and temporary files after interrupted publication
  or cleanup. A crash after a durable flush edit but before old-WAL removal can
  leave an obsolete WAL; M4 safely ignores it during recovery.
- Measurements: run from a clean committed tree. Capture output under ignored
  build paths, then copy the completed evidence into results before committing it.
