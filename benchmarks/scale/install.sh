#!/usr/bin/env bash
set -euo pipefail
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "run with sudo: sudo benchmarks/scale/install.sh" >&2
  exit 2
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  ca-certificates curl docker.io git jq make python3 python3-pip \
  python3-venv rsync
GO_ARCHIVE="go1.26.11.linux-amd64.tar.gz"
GO_SHA256="9849f4e04ee35303714e34fbc3ac8ab44ea3e448a95aa182f91e3b9b79c710ec"
mkdir -p /usr/local/src
curl --fail --location --silent --show-error --retry 5 \
  --output "/usr/local/src/$GO_ARCHIVE" "https://go.dev/dl/$GO_ARCHIVE"
echo "$GO_SHA256  /usr/local/src/$GO_ARCHIVE" | sha256sum --check -
rm -rf /usr/local/go
tar -C /usr/local -xzf "/usr/local/src/$GO_ARCHIVE"
rm -f "/usr/local/src/$GO_ARCHIVE"
ln -sf /usr/local/go/bin/go /usr/local/bin/go
ln -sf /usr/local/go/bin/gofmt /usr/local/bin/gofmt
GOTOOLCHAIN=local go version
systemctl enable --now docker
if [[ -n "${SUDO_USER:-}" ]]; then
  usermod -aG docker "$SUDO_USER"
fi
