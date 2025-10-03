#!/bin/bash

set -eux

VERSION="${1}"
BASEDIR="${2}"
ARCH="${3}"

# Clean version without 'v' prefix
CLEAN_VERSION=${VERSION#v}

# Debian package version format
DEB_VERSION="${CLEAN_VERSION}-1"
PACKAGE_NAME="pg-textsearch-postgresql-17"

# Setup directories
DEBDIR="${BASEDIR}/dist"
BUILDDIR="${BASEDIR}/debian-build"

rm -rf "${BUILDDIR}" "${DEBDIR}"
mkdir -p "${BUILDDIR}/DEBIAN"
mkdir -p "${DEBDIR}"

# Get PostgreSQL directories
# Support both Docker (TimescaleDB) and regular PostgreSQL installations
if [ -x "/usr/local/bin/pg_config" ]; then
    PG_CONFIG="/usr/local/bin/pg_config"
elif [ -x "/usr/lib/postgresql/17/bin/pg_config" ]; then
    PG_CONFIG="/usr/lib/postgresql/17/bin/pg_config"
else
    echo "Error: pg_config not found in expected locations"
    exit 1
fi
LIBDIR=$($PG_CONFIG --pkglibdir)
SHAREDIR=$($PG_CONFIG --sharedir)

# Create package directory structure
mkdir -p "${BUILDDIR}${LIBDIR}"
mkdir -p "${BUILDDIR}${SHAREDIR}/extension"

# Copy extension files
cp "${BASEDIR}/pgtextsearch.so" "${BUILDDIR}${LIBDIR}/" || \
   cp "${BASEDIR}/pgtextsearch.dylib" "${BUILDDIR}${LIBDIR}/" || \
   { echo "Error: Could not find pgtextsearch library"; exit 1; }

cp "${BASEDIR}/pgtextsearch.control" "${BUILDDIR}${SHAREDIR}/extension/"
cp "${BASEDIR}/sql/pgtextsearch--0.0.1.sql" "${BUILDDIR}${SHAREDIR}/extension/"

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
Depends: timescaledb-2-postgresql-17 | postgresql-17
Section: database
Priority: optional
Homepage: https://github.com/timescale/tapir
Description: pgtextsearch - PostgreSQL extension for full-text search with BM25
 pgtextsearch provides full-text search capabilities with BM25 ranking for
 PostgreSQL. It implements a memtable-based architecture similar to
 LSM trees, with in-memory structures that spill to disk segments
 for scalability.
EOF

# Create postinst script
cat > "${BUILDDIR}/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e

if [ "$1" = "configure" ]; then
    echo "pgtextsearch extension installed. Use 'CREATE EXTENSION pgtextsearch;' in PostgreSQL to enable."
fi

exit 0
EOF
chmod 755 "${BUILDDIR}/DEBIAN/postinst"

# Create prerm script
cat > "${BUILDDIR}/DEBIAN/prerm" << 'EOF'
#!/bin/sh
set -e

if [ "$1" = "remove" ]; then
    echo "Removing pgtextsearch extension. Please DROP EXTENSION pgtextsearch in all databases first."
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
