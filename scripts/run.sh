#!/usr/bin/env bash
set -euo pipefail

task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

command -v cmake >/dev/null || { printf 'Missing command: cmake\n'; exit 1; }
task_slint_dir="$(bash "$task_root/scripts/fetch-slint.sh")"

cmake -S "$task_root" -B "$task_root/build" \
    -DCMAKE_BUILD_TYPE=Release -DSlint_DIR="$task_slint_dir"
cmake --build "$task_root/build" --parallel 4
ctest --test-dir "$task_root/build" --output-on-failure
exec "$task_root/build/resend" "$@"
