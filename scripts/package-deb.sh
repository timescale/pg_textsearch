#!/bin/bash

set -eux

VERSION="${1}"
BASEDIR="${2}"
ARCH="${3}"
PG_VERSION="${4}"

# Clean version without 'v' prefix
CLEAN_VERSION=${VERSION#v}

# Debian package version format
DEB_VERSION="${CLEAN_VERSION}-1"
PACKAGE_NAME="pg-textsearch-postgresql-${PG_VERSION}"

# Setup directories
DEBDIR="${BASEDIR}/dist"
BUILDDIR="${BASEDIR}/debian-build"

rm -rf "${BUILDDIR}" "${DEBDIR}"
mkdir -p "${BUILDDIR}/DEBIAN"
mkdir -p "${DEBDIR}"

# Get PostgreSQL directories from the caller-selected installation.
PG_CONFIG="${PG_CONFIG:-pg_config}"
if ! command -v "${PG_CONFIG}" >/dev/null 2>&1; then
    echo "Error: PG_CONFIG '${PG_CONFIG}' is not executable"
    exit 1
fi
LIBDIR="$("${PG_CONFIG}" --pkglibdir)"
SHAREDIR="$("${PG_CONFIG}" --sharedir)"

# Create package directory structure
mkdir -p "${BUILDDIR}${LIBDIR}"
mkdir -p "${BUILDDIR}${SHAREDIR}/extension"
mkdir -p \
    "${BUILDDIR}${SHAREDIR}/extension/pg_textsearch/durable_compaction"
mkdir -p \
    "${BUILDDIR}/usr/share/doc/${PACKAGE_NAME}/durable_compaction"

# Copy extension files
cp "${BASEDIR}/pg_textsearch.so" "${BUILDDIR}${LIBDIR}/" || \
   cp "${BASEDIR}/pg_textsearch.dylib" "${BUILDDIR}${LIBDIR}/" || \
   { echo "Error: Could not find pg_textsearch library"; exit 1; }

cp "${BASEDIR}/pg_textsearch.control" "${BUILDDIR}${SHAREDIR}/extension/"
# Copy all SQL files for the extension
cp "${BASEDIR}"/sql/pg_textsearch*.sql "${BUILDDIR}${SHAREDIR}/extension/"
cp "${BASEDIR}"/scripts/durable_compaction/{01_setup_role.sql,02_wrapper.sql,03_backstop.sql} \
   "${BUILDDIR}${SHAREDIR}/extension/pg_textsearch/durable_compaction/"
cp "${BASEDIR}/scripts/durable_compaction/README.md" \
   "${BUILDDIR}/usr/share/doc/${PACKAGE_NAME}/durable_compaction/"
cp "${BASEDIR}/docs/background_compaction.md" \
   "${BUILDDIR}/usr/share/doc/${PACKAGE_NAME}/"

# Determine architecture
if [ "$ARCH" = "arm64" ]; then
    DEB_ARCH="arm64"
else
    DEB_ARCH="amd64"
fi

# Create control file
cat > "${BUILDDIR}/DEBIAN/control" << EOF
Package: ${PACKAGE_NAME}
Version: ${DEB_VERSION}
Architecture: ${DEB_ARCH}
Maintainer: Timescale <hello@timescale.com>
Depends: timescaledb-2-postgresql-${PG_VERSION} | postgresql-${PG_VERSION}
Section: database
Priority: optional
Homepage: https://github.com/timescale/pg_textsearch
Description: pg_textsearch - PostgreSQL extension for full-text search with BM25
 pg_textsearch provides full-text search capabilities with BM25 ranking for
 PostgreSQL. It implements a memtable-based architecture similar to
 LSM trees, with in-memory structures that spill to disk segments
 for scalability.
EOF

# Create postinst script
cat > "${BUILDDIR}/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e

if [ "$1" = "configure" ]; then
    echo "pg_textsearch extension installed. Use 'CREATE EXTENSION pg_textsearch;' in PostgreSQL to enable."
fi

exit 0
EOF
chmod 755 "${BUILDDIR}/DEBIAN/postinst"

# Create prerm script
cat > "${BUILDDIR}/DEBIAN/prerm" << 'EOF'
#!/bin/sh
set -e

if [ "$1" = "remove" ]; then
    echo "Removing pg_textsearch extension. Please DROP EXTENSION pg_textsearch in all databases first."
fi

exit 0
EOF
chmod 755 "${BUILDDIR}/DEBIAN/prerm"

# Build the package
cd "${BASEDIR}"
dpkg-deb --build debian-build

# Move to dist with proper name
PACKAGE_FILE="${PACKAGE_NAME}_${DEB_VERSION}_${DEB_ARCH}.deb"
mv debian-build.deb "${DEBDIR}/${PACKAGE_FILE}"

echo "Package created: ${DEBDIR}/${PACKAGE_FILE}"

# Cleanup
rm -rf "${BUILDDIR}"
