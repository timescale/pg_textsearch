#!/usr/bin/env bash

set -euo pipefail

version=${1:?PostgreSQL version is required}
prefix=${2:?installation prefix is required}
source_dir=${3:-"${RUNNER_TEMP:-/tmp}/postgresql-${version}"}

rm -rf "${source_dir}"
mkdir -p "${source_dir}"

curl -fsSL \
    "https://ftp.postgresql.org/pub/source/v${version}/postgresql-${version}.tar.bz2" |
    tar -xj -C "${source_dir}" --strip-components=1

cd "${source_dir}"
./configure \
    --prefix="${prefix}" \
    --enable-debug \
    --enable-cassert \
    --enable-injection-points \
    --without-icu \
    --without-readline \
    --without-zlib
make -j"$(nproc)"
make install
make -C src/test/modules/injection_points install
