#!/usr/bin/env bash
# scripts/quickstart.sh — the README quickstart, executable.
#
# R1: time-to-first-message under 10 minutes, quickstart tested in CI. The
# README documents THIS script's steps and CI executes THIS script from a
# clean checkout (.github/workflows/ci.yml, `quickstart` job), so the
# documented commands and the verified commands cannot drift.
#
# From a clean Ubuntu 22.04/24.04 checkout (git clone --recurse-submodules):
#   [1/4] apt dependencies (only what is missing; needs sudo)
#   [2/4] configure + build (tests on)
#   [3/4] first message: the M8 lib-only link example, run
#   [4/4] your topic visible from OUTSIDE the process: one POSIX-shm
#         publish, then `xmmsg list` shows it (R5), then `xmmsg clean`

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/build-quickstart"

echo "[1/4] dependencies (cmake, compiler, Eigen for the bundled xmBase)"
missing=()
command -v cmake >/dev/null 2>&1 || missing+=(cmake)
command -v g++ >/dev/null 2>&1 || missing+=(build-essential)
dpkg -s libeigen3-dev >/dev/null 2>&1 || missing+=(libeigen3-dev)
if [ "${#missing[@]}" -gt 0 ]; then
  sudo apt-get update
  sudo apt-get install -y "${missing[@]}"
fi

echo "[2/4] configure + build"
cmake -S "${root}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON
cmake --build "${build_dir}" -j"$(nproc)"

echo "[3/4] first message (M8: in-process publish/take, lib-only binary)"
"${build_dir}/test/link/xmmsg_libonly_link_test"

echo "[4/4] your topic, from outside the process (R5: xmmsg)"
# The full isolation key is user-derived (D17): u<uid>.<name>.
domain_key="u$(id -u).quickstart"
"${build_dir}/test/behavioral/shm_test_helper" \
    publish_once quickstart demo.plan.head 42
"${build_dir}/tools/xmmsg/xmmsg" list --domain "${domain_key}"
"${build_dir}/tools/xmmsg/xmmsg" list --domain "${domain_key}" \
    | grep -q "demo.plan.head"  # the topic really is listed
"${build_dir}/tools/xmmsg/xmmsg" clean --domain "${domain_key}" --yes >/dev/null

echo "quickstart: PASS — first message received and introspected"
