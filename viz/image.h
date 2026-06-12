/* image.h — RGB8 image buffer + PNG output (libpng) and a cheap diff metric
 * for golden-image visual-correctness checks. */
#ifndef ASTRA_IMAGE_H
#define ASTRA_IMAGE_H

#include <stdint.h>

typedef struct { int w, h; uint8_t *rgb; } Image;   /* rgb = w*h*3 bytes */

int  image_alloc(Image *im, int w, int h);
void image_free(Image *im);

/* glReadPixels gives bottom-up rows; this flips to top-down in place. */
void image_flip_y(Image *im);

int  image_write_png(const Image *im, const char *path);
int  image_read_png(Image *im, const char *path);   /* allocates im->rgb */

/* Mean absolute per-channel difference (0..255). <0 if sizes differ. */
double image_mad(const Image *a, const Image *b);

#endif /* ASTRA_IMAGE_H */
