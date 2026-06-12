/* image.c — RGB8 image + PNG I/O via libpng. */
#include "image.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <png.h>

int image_alloc(Image *im, int w, int h) {
    im->w = w; im->h = h;
    im->rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3u);
    return im->rgb != NULL;
}

void image_free(Image *im) {
    free(im->rgb); im->rgb = NULL; im->w = im->h = 0;
}

void image_flip_y(Image *im) {
    size_t row = (size_t)im->w * 3u;
    uint8_t *tmp = (uint8_t *)malloc(row);
    if (!tmp) return;
    for (int y = 0; y < im->h / 2; ++y) {
        uint8_t *a = im->rgb + (size_t)y * row;
        uint8_t *b = im->rgb + (size_t)(im->h - 1 - y) * row;
        memcpy(tmp, a, row); memcpy(a, b, row); memcpy(b, tmp, row);
    }
    free(tmp);
}

int image_write_png(const Image *im, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) png_destroy_write_struct(&png, info ? &info : NULL);
        fclose(fp); return 0;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)im->w, (png_uint_32)im->h, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < im->h; ++y)
        png_write_row(png, im->rgb + (size_t)y * (size_t)im->w * 3u);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 1;
}

int image_read_png(Image *im, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) png_destroy_read_struct(&png, info ? &info : NULL, NULL);
        fclose(fp); return 0;
    }
    png_init_io(png, fp);
    png_read_info(png, info);
    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    /* normalise to 8-bit RGB */
    png_set_strip_16(png);
    png_set_palette_to_rgb(png);
    png_set_expand_gray_1_2_4_to_8(png);
    png_set_strip_alpha(png);
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY)
        png_set_gray_to_rgb(png);
    png_read_update_info(png, info);
    if (!image_alloc(im, (int)w, (int)h)) {
        png_destroy_read_struct(&png, &info, NULL); fclose(fp); return 0;
    }
    for (png_uint_32 y = 0; y < h; ++y)
        png_read_row(png, im->rgb + (size_t)y * (size_t)w * 3u, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return 1;
}

double image_mad(const Image *a, const Image *b) {
    if (a->w != b->w || a->h != b->h) return -1.0;
    size_t n = (size_t)a->w * (size_t)a->h * 3u;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        int d = (int)a->rgb[i] - (int)b->rgb[i];
        acc += d < 0 ? -d : d;
    }
    return acc / (double)n;
}
