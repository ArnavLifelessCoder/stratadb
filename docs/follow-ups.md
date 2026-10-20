# Milestone follow-ups

- M5: exercise deletion of obsolete SSTables while a Windows mapping is still
  open. Verify that live readers retain their data and files are reclaimed once
  the owning versions and mappings are released.
- Measurements: run from a clean committed tree. Capture output under ignored
  build paths, then copy the completed evidence into results before committing it.
