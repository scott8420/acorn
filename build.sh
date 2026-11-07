#!/usr/bin/env bash
# Acorn build.
#   Fedora: sudo dnf install gtkmm4.0-devel spdlog-devel cmake gcc-c++ ninja-build
#   Ubuntu: sudo apt install libgtkmm-4.0-dev libspdlog-dev cmake g++ ninja-build
# Ninja is used when installed and the build dir is fresh; ccache / mold are
# picked up by CMake if present.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

gen=()
if [[ ! -f build/CMakeCache.txt ]] && command -v ninja >/dev/null 2>&1; then
    gen=(-G Ninja)
fi
launcher=()
if command -v ccache >/dev/null 2>&1; then
    launcher=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)
fi
linker=()
if command -v mold >/dev/null 2>&1; then
    linker=(-DCMAKE_LINKER_TYPE=MOLD)
fi

cmake -B build "${gen[@]}" "${launcher[@]}" "${linker[@]}"
cmake --build build --target acorn acorn_selftest -j"$(nproc)"
echo ""
echo "Built: ./build/acorn  and  ./build/acorn_selftest"
echo "Smoke: ./build/acorn_selftest"
