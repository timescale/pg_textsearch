#!/usr/bin/env bash
#
# bump-version.sh OLD NEW
#
# Bumps the pg_textsearch version from OLD to NEW. Run from the repo
# root with a clean working tree. Auto-detects mode from the version
# strings:
#
#   OLD=A.B.C       NEW=X.Y.Z-dev   →  dev bump (post-release)
#   OLD=X.Y.Z-dev   NEW=X.Y.Z       →  release (same triple)
#
# Updates the SQL files, Makefile DATA list, control file, mod.c,
# README, CLAUDE.md, test scripts, and the msmarco benchmark check.
# Does NOT regenerate the banner image, edit the upgrade-tests
# matrix, author the upgrade SQL, run tests, or open a PR — those
# are listed in the "next steps" output.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 OLD NEW" >&2
    exit 2
fi

OLD=$1
NEW=$2

# --- mode detection ------------------------------------------------

is_release() {  # X.Y.Z (no -dev suffix)
    [[ $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}
is_dev() {      # X.Y.Z-dev
    [[ $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+-dev$ ]]
}

if is_release "$OLD" && is_dev "$NEW"; then
    MODE=dev
elif is_dev "$OLD" && is_release "$NEW" && [[ ${OLD%-dev} == "$NEW" ]]; then
    MODE=release
else
    echo "Error: invalid OLD/NEW combination." >&2
    echo "  Dev bump: OLD=A.B.C, NEW=X.Y.Z-dev" >&2
    echo "  Release:  OLD=X.Y.Z-dev, NEW=X.Y.Z (same triple)" >&2
    exit 2
fi

# --- safety checks -------------------------------------------------

if [[ ! -f pg_textsearch.control ]]; then
    echo "Error: run from the repo root." >&2
    exit 2
fi

if [[ -n "$(git status --porcelain)" ]]; then
    echo "Error: working tree is not clean. Commit or stash first." >&2
    exit 2
fi

SRC_MAIN="sql/pg_textsearch--$OLD.sql"
DST_MAIN="sql/pg_textsearch--$NEW.sql"

if [[ ! -f $SRC_MAIN ]]; then
    echo "Error: $SRC_MAIN does not exist." >&2
    exit 2
fi
if [[ -f $DST_MAIN ]]; then
    echo "Error: $DST_MAIN already exists." >&2
    exit 2
fi

# --- mode-specific actions -----------------------------------------

if [[ $MODE == dev ]]; then
    NEW_UPGRADE="sql/pg_textsearch--$OLD--$NEW.sql"
    if [[ -f $NEW_UPGRADE ]]; then
        echo "Error: $NEW_UPGRADE already exists." >&2
        exit 2
    fi

    # Rename the base SQL file and rewrite version strings inside.
    git mv "$SRC_MAIN" "$DST_MAIN"
    perl -i -pe "s/\Q$OLD\E/$NEW/g" "$DST_MAIN"

    # Create the upgrade-script stub. Authors append feature DDL as
    # work lands; release-time audit verifies completeness.
    cat > "$NEW_UPGRADE" <<EOF
-- Upgrade from $OLD to $NEW

-- Verify loaded library matches this SQL script version
DO \$\$
DECLARE
    lib_ver text;
BEGIN
    lib_ver := pg_catalog.current_setting('pg_textsearch.library_version', true);
    IF lib_ver IS NULL THEN
        RAISE EXCEPTION
            'pg_textsearch library not loaded. '
            'Add pg_textsearch to shared_preload_libraries and restart.';
    END IF;
    IF lib_ver OPERATOR(pg_catalog.<>) '$NEW' THEN
        RAISE EXCEPTION
            'pg_textsearch library version mismatch: loaded=%, expected=%. '
            'Restart the server after installing the new binary.',
            lib_ver, '$NEW';
    END IF;
END \$\$;
EOF
    git add "$NEW_UPGRADE"

    # Makefile: rename the base-file entry and append the new
    # upgrade entry as a continuation line.
    perl -i -pe "s|pg_textsearch--\Q$OLD\E\.sql|pg_textsearch--$NEW.sql|g" Makefile
    perl -i -pe '
        if (/^(\s+sql\/pg_textsearch--[0-9].*\.sql)$/) {
            $_ = "$1 \\\n       sql/pg_textsearch--'"$OLD"'--'"$NEW"'.sql\n";
        }
    ' Makefile

elif [[ $MODE == release ]]; then
    # Find and rename the upgrade file `sql/pg_textsearch--PREV--OLD.sql`.
    # Globbing here uses bash's nullglob so we can count safely.
    shopt -s nullglob
    upgrades=( sql/pg_textsearch--*--"$OLD".sql )
    shopt -u nullglob
    if [[ ${#upgrades[@]} -ne 1 ]]; then
        echo "Error: expected exactly one sql/pg_textsearch--*--$OLD.sql," \
             "found ${#upgrades[@]}." >&2
        exit 2
    fi
    SRC_UPGRADE=${upgrades[0]}
    DST_UPGRADE=${SRC_UPGRADE//$OLD.sql/$NEW.sql}

    git mv "$SRC_MAIN" "$DST_MAIN"
    git mv "$SRC_UPGRADE" "$DST_UPGRADE"
fi

# --- common text substitutions -------------------------------------

# Files where every literal occurrence of OLD becomes NEW.
common_files=(
    pg_textsearch.control
    src/mod.c
    README.md
    CLAUDE.md
    test/scripts/memory_accounting.sh
    test/scripts/multi_index.sh
    test/scripts/segment_wal_recovery.sh
    benchmarks/datasets/msmarco-v2/run_v2_benchmark.sh
)

for f in "${common_files[@]}"; do
    if [[ -f $f ]]; then
        perl -i -pe "s/\Q$OLD\E/$NEW/g" "$f"
    fi
done

# --- straggler check -----------------------------------------------

# Find any remaining references to OLD outside known-safe locations.
# Excluded: this script, expected outputs, older upgrade SQL files,
# RELEASING.md (carries the historical upgrade chain), the
# upgrade-tests workflow (carries phase-history comments).
stragglers=$(git grep -l --fixed-strings "$OLD" -- \
    ':!scripts/bump-version.sh' \
    ':!test/expected/' \
    ':!sql/pg_textsearch--*--*.sql' \
    ':!RELEASING.md' \
    ':!.github/workflows/upgrade-tests.yml' \
    ':!docs/' \
    || true)

if [[ -n $stragglers ]]; then
    echo "Warning: literal '$OLD' still appears in:" >&2
    echo "$stragglers" | sed 's/^/  /' >&2
    echo "Review and update by hand if these need bumping." >&2
fi

# --- next steps ----------------------------------------------------

echo
echo "Version bump $OLD → $NEW complete."
echo

if [[ $MODE == release ]]; then
    PREV=${SRC_UPGRADE#sql/pg_textsearch--}
    PREV=${PREV%%--*}
    cat <<EOF
Next steps (release):
  1. Add the new banner image at images/tapir_and_friends_v$NEW.png
     and remove images/tapir_and_friends_v$OLD.png.
  2. Add $OLD to the old_version matrix in
     .github/workflows/upgrade-tests.yml.
  3. Audit sql/pg_textsearch--$PREV--$NEW.sql against the diff:
       diff sql/pg_textsearch--$PREV.sql sql/pg_textsearch--$NEW.sql
     Every new CREATE FUNCTION / OPERATOR / OPERATOR CLASS / TYPE /
     CAST in the main file must have a matching statement in the
     upgrade file. See RELEASING.md for the full audit checklist.
  4. Open a PR titled "Release v$NEW".
EOF
else
    cat <<EOF
Next steps (dev bump):
  1. Review the generated stub $NEW_UPGRADE.
  2. Open a PR titled "chore: bump version to $NEW".
EOF
fi
