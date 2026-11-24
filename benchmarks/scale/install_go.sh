#!/usr/bin/env bash

GO_VERSION="1.26.11"
GO_ARCHIVE="go${GO_VERSION}.linux-amd64.tar.gz"
GO_SHA256="9849f4e04ee35303714e34fbc3ac8ab44ea3e448a95aa182f91e3b9b79c710ec"

install_pinned_go() {
    local toolchains="${GO_TOOLCHAINS_DIR:-/opt/searchbench/toolchains}"
    local root="$toolchains/go${GO_VERSION}"
    local bin_dir="${GO_BIN_DIR:-/usr/local/bin}"
    local cache="${GO_DOWNLOAD_DIR:-/opt/searchbench/downloads}"
    local archive="${GO_ARCHIVE_FILE:-$cache/$GO_ARCHIVE}"
    local stage

    mkdir -p "$toolchains" "$bin_dir"
    if [[ ! -x "$root/bin/go" ]]; then
        if [[ -e "$root" ]]; then
            echo "Refusing incomplete pinned Go directory: $root" >&2
            return 1
        fi
        mkdir -p "$cache"
        if [[ -z "${GO_ARCHIVE_FILE:-}" ]]; then
            curl --fail --location --silent --show-error --retry 5 \
                --output "$archive" "https://go.dev/dl/$GO_ARCHIVE"
        fi
        printf '%s  %s\n' "$GO_SHA256" "$archive" | sha256sum --check -
        stage="$toolchains/.go${GO_VERSION}.installing.$$"
        rm -rf "$stage"
        mkdir -p "$stage"
        trap 'rm -rf "$stage"' RETURN
        tar -C "$stage" -xzf "$archive"
        [[ -x "$stage/go/bin/go" ]] || {
            echo "Pinned Go archive did not contain bin/go" >&2
            return 1
        }
        mv "$stage/go" "$root"
        rm -rf "$stage"
        trap - RETURN
        if [[ -z "${GO_ARCHIVE_FILE:-}" ]]; then
            rm -f "$archive"
        fi
    fi

    local version
    version="$(GOTOOLCHAIN=local "$root/bin/go" version)"
    [[ "$version" == "go version go${GO_VERSION} linux/amd64" ]] || {
        echo "Unexpected pinned Go version: $version" >&2
        return 1
    }

    local name target existing
    for name in go gofmt; do
        target="$root/bin/$name"
        existing="$bin_dir/$name"
        if [[ -e "$existing" || -L "$existing" ]]; then
            if [[ "$(readlink -f "$existing")" != \
                  "$(readlink -f "$target")" ]]; then
                echo "Refusing to replace foreign tool install: $existing" >&2
                return 1
            fi
        else
            ln -s "$target" "$existing"
        fi
    done
    GOTOOLCHAIN=local "$bin_dir/go" version
}
