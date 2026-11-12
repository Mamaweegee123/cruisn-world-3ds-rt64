#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <pinned-rt64-3ds-src/renderer> <build-dir>" >&2
    exit 2
fi

renderer_dir=$1
build_dir=$2
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

cmake -S "$project_dir" -B "$build_dir" \
    -DRT64_3DS_RENDERER_DIR="$renderer_dir" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_dir" --parallel 2
"$build_dir/cruisn_rt64_bridge_tests"
