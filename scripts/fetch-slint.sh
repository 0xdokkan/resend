#!/usr/bin/env bash
# Downloads and verifies the official Slint C++ SDK into .deps/, then prints
# the directory that contains SlintConfig.cmake. Messages go to stderr.
set -euo pipefail

task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
task_version="1.18.1"
task_archive="Slint-cpp-$task_version-Linux-x86_64.tar.gz"
# Digest published by GitHub for the release asset above.
task_sha256="f0b24ea601b80afe241677d1179bf5215dceae529f2b275b678a0af9a144b255"
task_deps="$task_root/.deps"
task_sdk="$task_deps/slint-$task_version"

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
    printf '%s\n' 'This SDK download supports Linux x86_64.' \
        'For other platforms, see README.md (CMake + Slint C++ SDK or RESEND_FETCH_SLINT).' >&2
    exit 1
fi

task_config=""
if [[ -d "$task_sdk" ]]; then
    task_config="$(find "$task_sdk" -name SlintConfig.cmake -print -quit)"
fi
if [[ -z "$task_config" ]]; then
    for task_command in curl tar sha256sum; do
        command -v "$task_command" >/dev/null || { printf 'Missing command: %s\n' "$task_command" >&2; exit 1; }
    done
    mkdir -p "$task_deps"
    task_temp="$(mktemp -d "$task_deps/slint-download.XXXXXX")"
    trap 'rm -rf -- "$task_temp"' EXIT
    printf 'Downloading official Slint C++ SDK %s into .deps/\n' "$task_version" >&2
    curl --fail --location --retry 2 --output "$task_temp/$task_archive" \
        "https://github.com/slint-ui/slint/releases/download/v$task_version/$task_archive"
    if ! printf '%s  %s\n' "$task_sha256" "$task_temp/$task_archive" | sha256sum --check --status; then
        printf 'Checksum mismatch for %s: the download was discarded.\n' "$task_archive" >&2
        exit 1
    fi
    mkdir -p "$task_sdk"
    tar -xzf "$task_temp/$task_archive" -C "$task_sdk"
    task_config="$(find "$task_sdk" -name SlintConfig.cmake -print -quit)"
    if [[ -z "$task_config" ]]; then
        printf '%s\n' 'The downloaded archive does not contain SlintConfig.cmake.' >&2
        exit 1
    fi
fi

dirname -- "$task_config"
