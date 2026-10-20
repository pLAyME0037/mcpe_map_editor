#!/bin/sh
# Builds ./mcgui — the GTK4 GUI (wiki/decisions.md#D2, #D48).
#
# build.c is not ours to change, so the GUI builds BESIDE the core build:
# this script compiles gui/*.c with GTK's flags and links them against the
# core objects ./rebuild.sh already made — everything but the CLI's main,
# vendored units included, the same -lm -lpthread -lz and the same debug
# sanitizers. Run ./rebuild.sh first (debug, the default). One flag set
# per side; neither side guesses at the other.
set -e
cd "$(dirname "$0")"

if [ ! -f build/obj/src_app_main.c.o ]; then
    echo "rebuild_gui.sh: no core objects. Run ./rebuild.sh first." >&2
    exit 1
fi

mkdir -p build/obj/gui
# Objects from sources that no longer exist would still link.
rm -f build/obj/gui/*.o

# GTK's include dirs as -isystem: their headers are not ours to lint.
gtk_cflags=$(pkg-config --cflags gtk4 | sed 's/-I/-isystem /g')
gtk_libs=$(pkg-config --libs gtk4)

for src in gui/*.c; do
    obj="build/obj/gui/$(basename "$src" .c).o"
    cc -std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -Iinclude -Igui $gtk_cflags -c "$src" -o "$obj"
done

core_objs=""
for obj in build/obj/src_*.o build/obj/third_party_*.o; do
    [ "$obj" = "build/obj/src_app_main.c.o" ] && continue
    core_objs="$core_objs $obj"
done

# shellcheck disable=SC2086
cc -fsanitize=address,undefined -o mcgui $core_objs build/obj/gui/*.o \
    $gtk_libs -lm -lpthread -lz

echo "built ./mcgui (debug, ASan+UBSan) — run: ./mcgui --selftest"
