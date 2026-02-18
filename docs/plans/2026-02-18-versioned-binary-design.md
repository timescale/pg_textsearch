# Versioned Binary for pg_textsearch

## Problem

The extension binary is always named `pg_textsearch.so`. When upgrading
via `make install`, the new .so overwrites the old one. If Postgres has
the old version memory-mapped, or if old and new are ABI-incompatible,
undefined behavior results (DLL Hell).

## Design

Following the PostGIS model, embed the full semver version in the binary
filename: `pg_textsearch-1.0.0-dev.so`.

### Source of Truth

`pg_textsearch.control`'s `default_version` field (already `1.0.0-dev`).

### Build Mechanism

The Makefile extracts the version and sets `MODULE_big`:

```makefile
EXTVERSION = $(shell awk -F"'" '/default_version/ {print $$2}' \
    pg_textsearch.control)
MODULE_big = pg_textsearch-$(EXTVERSION)
```

### Control File Install

The source `pg_textsearch.control` keeps an unversioned
`module_pathname = '$libdir/pg_textsearch'`. A custom Makefile rule
overrides PGXS's control file install to write the versioned path:

```
module_pathname = '$libdir/pg_textsearch-1.0.0-dev'
```

This avoids needing a `.control.in` template.

### Runtime Behavior

1. `CREATE EXTENSION pg_textsearch` reads the installed control file
2. Finds `module_pathname = '$libdir/pg_textsearch-1.0.0-dev'`
3. SQL functions using `MODULE_PATHNAME` resolve to this path
4. Postgres loads `pg_textsearch-1.0.0-dev.so`

### Upgrade Path

Old versions used `$libdir/pg_textsearch`. After upgrading:
- `make install` writes `pg_textsearch-1.0.0.so` alongside old
  `pg_textsearch.so`
- The updated control file's `module_pathname` points to the new .so
- `ALTER EXTENSION pg_textsearch UPDATE` loads the new binary
- Existing upgrade SQL scripts need no changes (they don't reference
  the library directly)

## Files Changed

| File | Change |
|------|--------|
| `Makefile` | Extract EXTVERSION, set MODULE_big, custom control install |
| `scripts/package-deb.sh` | Use versioned .so filename |
| `.github/workflows/ci.yml` | Check for versioned .so |
| `.github/workflows/release.yml` | Package versioned .so |
| `.github/workflows/package-release.yml` | Use versioned .so |
| `.github/workflows/upgrade-tests.yml` | Handle binary name transition |
| `RELEASING.md` | Document versioned binary in release checklist |
| `CLAUDE.md` | Add reference to RELEASING.md |

## Testing Strategy

1. **Build verification**: `make clean && make` produces
   `pg_textsearch-1.0.0-dev.so` (not `pg_textsearch.so`)
2. **Install verification**: `make install` places the versioned .so in
   `$libdir` and the installed control file has the versioned
   `module_pathname`
3. **Regression tests**: `make test` passes (all SQL tests use
   `MODULE_PATHNAME` which resolves from the control file)
4. **Manual smoke test**: `CREATE EXTENSION pg_textsearch` in a fresh
   database, verify functions load from the versioned .so
5. **CI validation**: Workflows check for the versioned binary name
