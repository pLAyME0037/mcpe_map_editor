/*
 * stb_image and stb_image_write implementation units.
 *
 * As stb_impl.c anticipated when the texture pipeline was still a
 * promise: the image code compiles in only once the texture layer
 * uses it (D32). Exactly one translation unit defines the
 * implementation macros; every other file includes the headers
 * and calls the functions.
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
