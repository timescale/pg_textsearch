#!/bin/bash
set -euxo pipefail
cd "$(dirname "$0")"
EXPECTED_COMPUTE_ID="${1:?expected Azure compute resource ID is required}"
python3 - "$EXPECTED_COMPUTE_ID" <<'PY'
import sys
from bootstrap_disks import require_benchmark_vm
require_benchmark_vm(sys.argv[1])
PY
source ./install_go.sh
if [[ -f /opt/searchbench/bootstrap.ready ]]; then
    install_pinned_go
    mountpoint -q /srv/searchbench
    python3 -c \
        'import json; '\
'assert json.load(open("/etc/docker/daemon.json"))["data-root"] == '\
'"/srv/searchbench/docker"'
    systemctl start docker
    test "$(docker info --format '{{.DockerRootDir}}')" = \
        /srv/searchbench/docker
    exit 0
fi
export DEBIAN_FRONTEND=noninteractive
apt-get -o DPkg::Lock::Timeout=600 update -qq
apt-get -o DPkg::Lock::Timeout=600 install -y -qq \
    docker.io git curl jq python3-venv python3-pip \
    build-essential mdadm nvme-cli sysstat linux-tools-common \
    postgresql-client rsync zstd
install_pinned_go
python3 bootstrap_disks.py --expected-compute-id "$EXPECTED_COMPUTE_ID" \
    --initialize-ephemeral-storage
systemctl stop docker.service docker.socket
if [[ -f /etc/docker/daemon.json ]]; then
    python3 -c \
        'import json; '\
'assert json.load(open("/etc/docker/daemon.json"))["data-root"] == '\
'"/srv/searchbench/docker"'
else
    printf '%s\n' '{"data-root":"/srv/searchbench/docker"}' \
        >/etc/docker/daemon.json
fi
systemctl enable --now docker
usermod -aG docker bench
install -d -o bench -g bench /opt/searchbench
cat >/etc/sysctl.d/90-searchbench.conf <<'EOF'
vm.max_map_count=262144
EOF
sysctl --system
touch /opt/searchbench/bootstrap.ready
