#!/usr/bin/env bash
# Runs the canvas spike benchmarks on the current display and prints one
# line per run: renderer, scenario, cable drawing, FPS (min/avg, first second
# skipped) and the average C++ cost per input update.
# Usage: spikes/canvas/bench.sh path/to/canvas-spike [seconds]
set -euo pipefail

task_binary="$1"
task_seconds="${2:-4}"

task_run() {
    local renderer="$1" scenario="$2" paths="$3" output fps
    local flags=(--bench "$scenario" --seconds "$task_seconds")
    [[ "$paths" == per-link ]] && flags+=(--per-link)
    output="$(SLINT_BACKEND="$renderer" SLINT_DEBUG_PERFORMANCE=refresh_full_speed,console \
        timeout 60 "$task_binary" "${flags[@]}" 2>&1)"
    fps="$(printf '%s\n' "$output" | sed -n 's/^average frames per second: *\([0-9]*\).*/\1/p' | tail -n +2 |
        awk 'NR == 1 { min = $1 } { sum += $1; if ($1 < min) min = $1 } END { if (NR) printf "%d/%d", min, sum / NR; else print "n/a" }')"
    printf '%-14s %-7s %-9s fps(min/avg)=%-8s %s\n' "$renderer" "$scenario" "$paths" "$fps" \
        "$(printf '%s\n' "$output" | grep -o 'avg_update_us=[0-9.]*' || true)"
}

for task_renderer in winit-skia winit-femtovg winit-software; do
    for task_scenario in idle drag pan zoom scroll; do
        task_run "$task_renderer" "$task_scenario" single
    done
    for task_scenario in drag pan zoom; do
        task_run "$task_renderer" "$task_scenario" per-link
    done
done
