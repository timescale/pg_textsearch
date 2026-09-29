#!/usr/bin/env bash

set -euo pipefail

version=${1:?PostgreSQL version is required}
source_dir=${2:?source directory is required}
prefix=${3:?installation prefix is required}
randomize_memory=${4:-false}
build_walinspect=${5:-false}
repo_root=$(pwd)

rm -rf "${source_dir}"
mkdir -p "${source_dir}"

wget -qO- \
    "https://ftp.postgresql.org/pub/source/v${version}/postgresql-${version}.tar.bz2" |
    tar -xj -C "${source_dir}" --strip-components=1

major=${version%%.*}
if ((major < 18)); then
	patch_file="${repo_root}/test/postgres-asan-instrumentation.patch"
else
	patch_file="${repo_root}/test/postgres-asan-instrumentation-PG18GE.patch"
fi
patch -F5 -p1 -d "${source_dir}" <"${patch_file}"

cd "${source_dir}"
configure_args=(
	"--prefix=${prefix}"
	--enable-debug
	--enable-cassert
	--enable-injection-points
	--with-openssl
	--without-readline
	--without-zlib
	--without-libxml
)

if [[ "${randomize_memory}" == "true" ]]; then
	CPPFLAGS="-DRANDOMIZE_ALLOCATED_MEMORY" ./configure "${configure_args[@]}"
else
	./configure "${configure_args[@]}"
fi

make -j"$(nproc)"
make -C src/test/modules/injection_points -j"$(nproc)"

if [[ "${build_walinspect}" == "true" ]]; then
	make -C contrib/pg_walinspect -j"$(nproc)"
fi
