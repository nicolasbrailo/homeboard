#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

// Load jpeg after std*, it doesn't include on its own
#include <jpeglib.h>

#include "jpeg_loader.h"

struct jpeg_err_ctx {
  struct jpeg_error_mgr pub;
  jmp_buf jmp;
};

static void jpeg_err_exit(j_common_ptr cinfo) {
  struct jpeg_err_ctx *e = (struct jpeg_err_ctx *)cinfo->err;
  char msg[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, msg);
  fprintf(stderr, "libjpeg error: %s\n", msg);
  longjmp(e->jmp, 1);
}

static struct jpeg_image *jpeg_load_file(FILE *f, const char *src_name, uint32_t target_w, uint32_t target_h) {
  struct jpeg_image *img = malloc(sizeof(struct jpeg_image));
  if (!img) {
    fclose(f);
    return NULL;
  }
  img->pixels = NULL;

  struct jpeg_decompress_struct cinfo;
  struct jpeg_err_ctx jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_err_exit;

  // libjpeg errors seem to call abort in some situations, this should override it and make it recoverable
  if (setjmp(jerr.jmp)) {
    // libjpeg signaled a fatal error; clean up and bail instead of exit()ing.
    fprintf(stderr, "jpeg decode aborted: %s\n", src_name);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    free(img->pixels);
    free(img);
    return NULL;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, f);

  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    fprintf(stderr, "bad jpeg header: %s\n", src_name);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    free(img);
    return NULL;
  }

  cinfo.out_color_space = JCS_RGB;

  // Decode to the size that's closest to our display by picking the largest denominator where output still covers the
  // target
  if (target_w > 0 && target_h > 0) {
    static const unsigned denoms[] = {8, 4, 2, 1};
    for (int i = 0; i < 4; i++) {
      unsigned d = denoms[i];
      if (cinfo.image_width / d >= target_w && cinfo.image_height / d >= target_h) {
        cinfo.scale_num = 1;
        cinfo.scale_denom = d;
        break;
      }
    }
  }

  jpeg_start_decompress(&cinfo);
  img->width = cinfo.output_width;
  img->height = cinfo.output_height;
  uint32_t row_bytes = img->width * 3;
  img->pixels = malloc(row_bytes * img->height);
  if (!img->pixels) {
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    free(img);
    return NULL;
  }

  while (cinfo.output_scanline < cinfo.output_height) {
    uint8_t *row = img->pixels + cinfo.output_scanline * row_bytes;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  fclose(f);
  return img;
}

struct jpeg_image *jpeg_load(const char *path, uint32_t target_w, uint32_t target_h) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return NULL;
  }
  return jpeg_load_file(f, path, target_w, target_h);
}

struct jpeg_image *jpeg_load_fd(int fd, uint32_t target_w, uint32_t target_h) {
  FILE *f = fdopen(fd, "rb");
  if (!f) {
    perror("fdopen");
    close(fd);
    return NULL;
  }
  return jpeg_load_file(f, "<fd>", target_w, target_h);
}

void jpeg_free(struct jpeg_image *img) {
  if (!img)
    return;
  free(img->pixels);
  free(img);
}
