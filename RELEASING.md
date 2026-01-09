# Releasing pg_textsearch

This document describes the release process for pg_textsearch.

## Version Scheme

We use semantic versioning: `MAJOR.MINOR.PATCH`

- Development versions use `-dev` suffix (e.g., `0.3.0-dev`)
- Release versions drop the suffix (e.g., `0.3.0`)

## Release Checklist

### 1. Update Version References

Change version from `X.Y.Z-dev` to `X.Y.Z` in these files:

| File | What to update |
|------|----------------|
| `pg_textsearch.control` | `default_version` |
| `Makefile` | `DATA` list (SQL filenames) |
| `README.md` | Status line, image reference |
| `CLAUDE.md` | `Current Version` |

### 2. Update SQL Files

**Rename the main SQL file:**
```sh
git mv sql/pg_textsearch--X.Y.Z-dev.sql sql/pg_textsearch--X.Y.Z.sql
```

**Rename the upgrade script:**
```sh
git mv sql/pg_textsearch--W.V.U--X.Y.Z-dev.sql sql/pg_textsearch--W.V.U--X.Y.Z.sql
```

**Create dev-to-release upgrade path** (for users on dev version):
```sh
# Create sql/pg_textsearch--X.Y.Z-dev--X.Y.Z.sql with:
-- Upgrade from X.Y.Z-dev to X.Y.Z
-- No schema changes

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to vX.Y.Z';
END
$$;
```

**Update SQL file contents:**
- In the main SQL file, update the version comment at the top
- Update the `RAISE INFO` message (remove prerelease warnings for releases)

### 3. Update Makefile DATA List

Add the new upgrade script to the `DATA` list:
```makefile
DATA = sql/pg_textsearch--X.Y.Z.sql \
       ... \
       sql/pg_textsearch--W.V.U--X.Y.Z.sql \
       sql/pg_textsearch--X.Y.Z-dev--X.Y.Z.sql
```

### 4. Rename Release Banner Image

```sh
git mv images/tapir_and_friends_vX.Y.Z-dev.png images/tapir_and_friends_vX.Y.Z.png
```

Update the image reference in README.md.

### 5. Update README Status

Update the status line to reflect release state. Examples:

```markdown
# For early releases:
🚀 **Status**: v0.3.0 - Query performance is production-ready. Index compression
and parallel index builds are coming soon. See [ROADMAP.md](ROADMAP.md) for details.

# For stable releases:
🚀 **Status**: v1.0.0 - Production ready. See [ROADMAP.md](ROADMAP.md) for upcoming features.
```

Remove any "Now Open Source" or similar one-time announcements from previous releases.

### 6. Run Tests and Update Expected Output

```sh
make clean && make && make install
make test
```

If INFO messages changed, regenerate expected outputs:
```sh
make expected
```

Don't forget alternative expected files (`*_1.out`) which need manual updates.

Verify all tests pass:
```sh
make test  # Should show "All N tests passed"
```

### 7. Create PR

```sh
git checkout -b release-X.Y.Z
git add -A
git commit -m "Release vX.Y.Z"
git push -u origin release-X.Y.Z
gh pr create --title "Release vX.Y.Z" --body "## Summary
- Update version from X.Y.Z-dev to X.Y.Z
- [other changes]

## Testing
- \`make test\` passes
- \`make format-check\` passes"
```

### 8. After PR Merges

The release workflow (`release.yml`) triggers automatically when a PR with title
starting "Release v" merges to main. It:

1. Creates a git tag
2. Builds release artifacts for PG17/PG18 on Linux/macOS (amd64/arm64)
3. Creates a GitHub release with the artifacts

### 9. Post-Release: Bump to Next Dev Version

After the release is published, create a PR to bump to the next dev version:

```sh
# Example: after releasing 0.3.0, bump to 0.4.0-dev
git checkout main && git pull
git checkout -b bump-to-0.4.0-dev
```

Update files with `0.4.0-dev`, create new SQL files, etc.

## SQL Upgrade Path Requirements

Every release must have upgrade paths from:

1. **Previous stable release** → new release (e.g., `0.2.0--0.3.0.sql`)
2. **Previous dev version** → new release (e.g., `0.3.0-dev--0.3.0.sql`)

This ensures users can upgrade regardless of which version they installed.

## Automated Workflows

| Workflow | Trigger | Purpose |
|----------|---------|---------|
| `ci.yml` | PR, push to main | Build and test on PG17/18 |
| `upgrade-tests.yml` | PR (sql/control changes), weekly | Test extension upgrades |
| `release.yml` | PR merge with "Release v" title | Create release and artifacts |
| `benchmark.yml` | Weekly, manual | Performance regression testing |

## Troubleshooting

### Old SQL files in Postgres share directory

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
1. The upgrade SQL file exists in the share directory
2. The control file has the correct `default_version`
3. The upgrade path exists (check `pg_extension_update_paths('pg_textsearch')`)
