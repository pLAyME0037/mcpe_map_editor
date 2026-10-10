#!/bin/sh
# Bootstraps the nob build tool and runs it.
#   ./rebuild.sh           -> debug build of ./mcme (ASan+UBSan)
#   ./rebuild.sh release   -> optimized build
#   ./rebuild.sh test      -> build and run unit tests
#   ./rebuild.sh fuzz      -> build libFuzzer harnesses (needs clang)
set -e
cd "$(dirname "$0")"

if [ ! -f third_party/nob/nob.h ]; then
    echo "rebuild.sh: third_party/nob/nob.h missing. Run the vendor step first." >&2
    exit 1
fi

# nob is its own build tool: compile build.c (which #includes nob.h with
# NOB_IMPLEMENTATION) into ./nob, then let it build the real thing.
cc -std=gnu11 -O1 -g -Wall -Wextra -o nob build.c

exec ./nob "$@"
