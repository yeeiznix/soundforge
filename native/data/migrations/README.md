# Migrations

Schema migration descriptors. G0 ships one hard-coded migrator (v0→1) in
`native/src/core/migration.cpp`; `manifest.json` declares the supported
paths. Future schema versions append a descriptor here and a matching
step in the C++ (and Python mirror) migrator.
