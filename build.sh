#!/usr/bin/env bash
# Build moyou on Linux. Needs a C++20 compiler, CMake >= 3.25 and Ninja.
# With VCPKG_ROOT set it builds the dependencies from the vcpkg manifest; without it, the distribution's
# Qt 6 (>= 6.5) and zlib development packages are used. On Fedora: qt6-qtbase-devel qt6-qtbase-private-devel
# zlib-ng-compat-devel.
set -euo pipefail
cd "$(dirname "$0")"
preset="${1:-${VCPKG_ROOT:+linux}}"
preset="${preset:-linux-system}"
cmake --preset "$preset"
cmake --build --preset "$preset"
echo "Built: build/$preset/moyou"
