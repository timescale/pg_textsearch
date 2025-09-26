#!/bin/bash

set -eux

VERSION="${1}"
BASEDIR="${2}"
ARCH="${3}"

# Clean version without 'v' prefix
CLEAN_VERSION=${VERSION#v}

# Debian package version format
DEB_VERSION="${CLEAN_VERSION}-1"
PACKAGE_NAME="tapir-postgresql-17"

# Setup directories
DEBDIR="${BASEDIR}/dist"
BUILDDIR="${BASEDIR}/debian-build"

rm -rf "${BUILDDIR}" "${DEBDIR}"
mkdir -p "${BUILDDIR}/DEBIAN"
mkdir -p "${DEBDIR}"

# Get PostgreSQL directories
PG_CONFIG="/usr/lib/postgresql/17/bin/pg_config"
LIBDIR=$($PG_CONFIG --pkglibdir)
SHAREDIR=$($PG_CONFIG --sharedir)

# Create package directory structure
mkdir -p "${BUILDDIR}${LIBDIR}"
mkdir -p "${BUILDDIR}${SHAREDIR}/extension"

# Copy extension files
cp "${BASEDIR}/tapir.so" "${BUILDDIR}${LIBDIR}/" || \
   cp "${BASEDIR}/tapir.dylib" "${BUILDDIR}${LIBDIR}/" || \
   { echo "Error: Could not find tapir library"; exit 1; }

cp "${BASEDIR}/tapir.control" "${BUILDDIR}${SHAREDIR}/extension/"
cp "${BASEDIR}/sql/tapir--0.0.sql" "${BUILDDIR}${SHAREDIR}/extension/"

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
Depends: postgresql-17
Section: database
Priority: optional
Homepage: https://github.com/timescale/tapir
Description: Tapir - PostgreSQL extension for full-text search with BM25
 Tapir provides full-text search capabilities with BM25 ranking for
 PostgreSQL. It implements a memtable-based architecture similar to
 LSM trees, with in-memory structures that spill to disk segments
 for scalability.
EOF

# Create postinst script
cat > "${BUILDDIR}/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e

if [ "$1" = "configure" ]; then
    echo "Tapir extension installed. Use 'CREATE EXTENSION tapir;' in PostgreSQL to enable."
fi

exit 0
EOF
chmod 755 "${BUILDDIR}/DEBIAN/postinst"

# Create prerm script
cat > "${BUILDDIR}/DEBIAN/prerm" << 'EOF'
#!/bin/sh
set -e

if [ "$1" = "remove" ]; then
    echo "Removing Tapir extension. Please DROP EXTENSION tapir in all databases first."
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
