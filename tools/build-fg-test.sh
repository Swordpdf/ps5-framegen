#!/usr/bin/env bash
# build-fg-test.sh [OUT] [INC_DIR]: the host test of the frame generation (tools/fg-test.cpp + src/ps5_framegen.cpp) against the
# system's Vulkan loader. INC_DIR: a folder with framegen/fg_shaders.inc to use instead of src/'s (a variant from build-shaders.sh).
# Needs a C++17 compiler (CXX, default clang++), the Khronos Vulkan headers (VULKAN_HEADERS, default /usr/include) and libvulkan.so.1.
# Run it with VK_ICD_FILENAMES set to a software Vulkan driver's ICD file (SwiftShader's vk_swiftshader_icd.json, which ships with
# Chromium builds), or on any Vulkan GPU.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build/fg-test}
inc=${2:-$root/src}
mkdir -p "$(dirname "$out")"
cxx=${CXX:-clang++}
vkh=${VULKAN_HEADERS:-/usr/include}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# ps5_framegen.cpp includes "framegen/fg_shaders.inc" next to itself: a copy beside the variant's include
cp "$root/src/ps5_framegen.cpp" "$root/src/ps5_framegen.h" "$work/"
mkdir -p "$work/framegen"
cp "$inc/framegen/fg_shaders.inc" "$work/framegen/"
$cxx -std=c++17 -O2 -g -Wall -Wno-missing-field-initializers -I"$work" -I"$vkh" -I"$root/tools/stb" \
	"$root/tools/fg-test.cpp" "$work/ps5_framegen.cpp" -o "$out" -l:libvulkan.so.1 -ldl
echo "built $out"
