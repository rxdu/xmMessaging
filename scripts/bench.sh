#!/usr/bin/env bash
# scripts/bench.sh — the M9-A1 one command.
#
# Configures build-bench at -O2 -DNDEBUG, builds the bench binary, runs the
# full suite, writes bench/results/<timestamp>.json (machine-readable report
# with embedded hardware context), prints the human summary, and runs the
# M9-A5 comparison against bench/reference.json (report-only until the
# reference is pinned; set XMMSG_BENCH_GATE=1 to gate once pinned).
#
# Usage: scripts/bench.sh [--smoke] [--filter <substr>]

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/build-bench"

cmake -S "${root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DXMMESSAGING_BUILD_BENCH=ON \
    -DXMMESSAGING_BUILD_SCENARIOS=OFF
cmake --build "${build_dir}" --target xmmsg_bench -j"$(nproc)"

mkdir -p "${root}/bench/results"
out="${root}/bench/results/$(date -u +%Y%m%dT%H%M%SZ).json"

"${build_dir}/bench/xmmsg_bench" --out "${out}" "$@"

python3 "${root}/bench/compare.py" "${out}" "${root}/bench/reference.json" \
    ${XMMSG_BENCH_GATE:+--gate}

echo "bench report: ${out}"
