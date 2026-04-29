# Releasing pg_textsearch

This document describes the release process for pg_textsearch.

## Version Scheme

We use semantic versioning: `MAJOR.MINOR.PATCH`.

- Development versions use `-dev` suffix (e.g., `1.2.0-dev`).
- Release versions drop the suffix (e.g., `1.2.0`).

## Cutting a Release

### 1. Audit the upgrade SQL script

This is the only piece of the release that cannot be automated, and the
piece most likely to be wrong. Skim it carefully.

The upgrade script `sql/pg_textsearch--PREV--CURRENT-dev.sql` must
recreate, for an installation still on `PREV`, every catalog object
that exists in the current main SQL file but not in the previous
release's main SQL file. To enumerate what the upgrade script must
cover, diff the two main SQL files:

```sh
git show vPREV:sql/pg_textsearch--PREV.sql > /tmp/prev.sql
diff /tmp/prev.sql sql/pg_textsearch--CURRENT-dev.sql
```

For every statement that is new in the current main file, verify
the upgrade script has a matching statement:

| New in main file              | Required in upgrade script        |
|-------------------------------|-----------------------------------|
| `CREATE FUNCTION`             | `CREATE FUNCTION`                 |
| `CREATE OPERATOR`             | `CREATE OPERATOR`                 |
| `CREATE OPERATOR CLASS`       | `CREATE OPERATOR CLASS`           |
| `CREATE TYPE`                 | `CREATE TYPE`                     |
| `CREATE CAST`                 | `CREATE CAST`                     |
| `ALTER OPERATOR FAMILY`       | `ALTER OPERATOR FAMILY`           |
| Catalog `UPDATE pg_catalog.*` | Same `UPDATE`                     |

Renamed, signature-changed, or removed objects need matching `DROP` /
`ALTER` statements in the upgrade script.

The `upgrade-tests` CI workflow exercises the upgrade against every
version in its `old_version` matrix and is the primary safety net for
gaps — but it can only catch what the regression suite touches. A new
operator that has no test coverage will pass CI and ship broken. The
audit catches what the workflow can't.

### 2. Run the bump script

```sh
./scripts/bump-version.sh CURRENT-dev CURRENT
```

This renames the SQL files and updates every version reference across
the tree. The script reports what's left for the human to do.

### 3. Update the release banner image

Replace `images/tapir_and_friends_vCURRENT-dev.png` with the new
release banner at `images/tapir_and_friends_vCURRENT.png`. (The
README reference is already updated by the bump script.)

### 4. Add the previous release to the upgrade-tests matrix

In `.github/workflows/upgrade-tests.yml`, add `PREV` to the
`old_version` matrix so future releases are tested for upgrade
compatibility from this version.

### 5. Open the PR

```sh
git checkout -b release-CURRENT
git add -A
git commit -m "Release vCURRENT"
gh pr create --draft --title "Release vCURRENT"
```

CI runs the full test suite, including upgrade-tests against every
matrix version.

### 6. After PR merges

The `release.yml` workflow triggers on merge of any PR titled
`Release v*`. It tags the commit, builds release artifacts for
PG17/PG18 on Linux/macOS (amd64/arm64), and publishes a GitHub
release.

## Bumping to the Next Dev Version

After a release is published, open a follow-up PR:

```sh
git checkout main && git pull
git checkout -b bump-to-NEXT-dev
./scripts/bump-version.sh CURRENT NEXT-dev
git add -A
git commit -m "chore: bump version to NEXT-dev"
gh pr create --draft --title "chore: bump version to NEXT-dev"
```

The script creates the new main SQL file (renamed from the previous
release's), creates a stub upgrade file `sql/pg_textsearch--CURRENT--NEXT-dev.sql`
containing only the library-version check, and updates every other
version reference. Contributors then append to the upgrade file as
features land in the dev cycle.

## SQL Upgrade Path Requirements

**Upgrade scripts must form a single linear chain.** Every version
connects to exactly one next version — no shortcuts that skip
intermediate steps. This keeps the number of upgrade scripts minimal
and the path predictable.

```
... → 0.5.0 → 0.5.1 → 0.6.0 → 0.6.1 → 1.0.0 → 1.1.0
```

Every release must have an upgrade path from the previous stable
release (e.g., `1.0.0--1.1.0.sql`). Dev versions are not supported for
direct upgrades; users on dev versions should reinstall the extension.

## Upgrade Compatibility

Not all version upgrades are compatible with `ALTER EXTENSION UPDATE`.
Breaking changes that require index recreation or server restart:

| Change Type | Impact | User Action Required |
|-------------|--------|---------------------|
| Metapage version bump | Existing indexes incompatible | `REINDEX` or recreate index |
| Shared memory structure change | Server crash on mixed versions | Restart Postgres after upgrade |
| Segment format change | Old segments unreadable | Recreate index |

**Current compatibility matrix:**

| From Version | To Version | Compatible? | Notes |
|--------------|------------|-------------|-------|
| 0.5.x | 0.6.0 | ⚠️ REINDEX | Segment format v3→v4 (uint32→uint64 offsets) |
| 0.3.0 | 0.4.0 | ❌ No | Segment format v2→v3 for compression |
| 0.2.0 | 0.4.0 | ❌ No | Segment format v2→v3 |
| 0.2.0 | 0.3.0 | ✅ Yes | Direct upgrade works |
| 0.1.0 | 0.3.0 | ❌ No | Metapage v4→v5, shmem size change |
| < 0.1.0 | 0.3.0 | ❌ No | Multiple breaking changes |

When releasing a version with breaking changes:

1. Update `.github/workflows/upgrade-tests.yml` to exclude incompatible versions.
2. Document in release notes that users must recreate indexes.
3. Consider providing a migration guide for major version bumps.

## On-disk Format Versions

If any on-disk format changed during a release cycle, bump the
corresponding version constant:

| Constant | Header | Purpose |
|----------|--------|---------|
| `TP_METAPAGE_VERSION` | `src/constants.h` | Index metapage format |
| `TP_DOCID_PAGE_VERSION` | `src/constants.h` | Document ID page format |
| `TP_PAGE_INDEX_VERSION` | `src/constants.h` | Page index format |
| `TP_SEGMENT_FORMAT_VERSION` | `src/segment/segment.h` | Segment block storage format |
| `TPQUERY_VERSION` | `src/types/query.h` | bm25query binary format |

Version bumps break upgrade compatibility — exclude incompatible old
versions from the upgrade-tests matrix and document the breaking
change in release notes.

## Automated Workflows

| Workflow | Trigger | Purpose |
|----------|---------|---------|
| `ci.yml` | PR, push to main | Build and test on PG17/18 |
| `upgrade-tests.yml` | PR (sql/control changes), weekly | Test extension upgrades |
| `release.yml` | PR merge with "Release v" title | Create release and artifacts |
| `benchmark.yml` | Weekly, manual | Performance regression testing |

## Troubleshooting

### Old SQL files in the Postgres share directory

If tests fail with old version messages, check for stale files:

```sh
ls ~/pg18/share/postgresql/extension/pg_textsearch*
```

Remove old dev versions that shouldn't be installed:

```sh
rm ~/pg18/share/postgresql/extension/pg_textsearch--X.Y.Z-dev.sql
```

### Extension won't upgrade

If `ALTER EXTENSION pg_textsearch UPDATE` fails, verify:

1. The upgrade SQL file exists in the share directory.
2. The control file has the correct `default_version`.
3. The upgrade path exists (check `pg_extension_update_paths('pg_textsearch')`).
