/*
 * stb_ds implementation unit.
 *
 * stb headers are header-only; exactly one translation unit must define the
 * implementation macro. Keeping it here means every other file can just
 * #include "stb_ds.h" and use arrput()/arrlen()/shput()/shget().
 *
 * stb_image and stb_image_write get their own unit under src/resource/ when
 * the texture pipeline lands (Phase 5) — we do not compile in image code we
 * do not use yet.
 */

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"
