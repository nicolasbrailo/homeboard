#include "eink_meta.h"

#include "eink/eink.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct EinkMeta {
  struct EInkDisplay *display;
  char last_text[256];
};

struct EinkMeta *eink_meta_init(void) {
  struct EinkMeta *em = calloc(1, sizeof(*em));
  if (!em)
    return NULL;
  struct EInkConfig cfg = {0};
  em->display = eink_init(&cfg);
  if (!em->display) {
    fprintf(stderr, "eink_meta: eink_init failed\n");
    free(em);
    return NULL;
  }
  return em;
}

void eink_meta_free(struct EinkMeta *em) {
  if (!em)
    return;
  eink_clear(em->display);
  if (em->display)
    eink_delete(em->display);
  free(em);
}

void eink_meta_clear(struct EinkMeta *em) { eink_clear(em->display); }

void eink_meta_render(struct EinkMeta *em, const char *meta_json) {
  if (!em || !meta_json)
    return;

  struct json_object *root = json_tokener_parse(meta_json);
  if (!root) {
    fprintf(stderr, "eink_meta: failed to parse metadata json\n");
    return;
  }

  {
    struct json_object *obj = NULL;
    if (json_object_object_get_ex(root, "local_path", &obj)) {
      printf("Displaying metadata for %s\n", json_object_get_string(obj));
    } else if (json_object_object_get_ex(root, "albumpath", &obj)) {
      printf("Displaying metadata for %s", json_object_get_string(obj));
      if (json_object_object_get_ex(root, "filename", &obj)) {
        printf("%s", json_object_get_string(obj));
      }
      printf("\n");
    }
  }

  const char *city = NULL;
  struct json_object *revgeo = NULL;
  if (json_object_object_get_ex(root, "reverse_geo", &revgeo)) {
    struct json_object *city_obj = NULL;
    if (json_object_object_get_ex(revgeo, "city", &city_obj))
      city = json_object_get_string(city_obj);
  }

  char year[5] = {0};
  struct json_object *dto = NULL;
  if (json_object_object_get_ex(root, "EXIF DateTimeOriginal", &dto)) {
    const char *s = json_object_get_string(dto);
    if (s && strlen(s) >= 4)
      memcpy(year, s, 4);
  }

  char text[256];
  if (city && year[0])
    snprintf(text, sizeof(text), "%s, %s", city, year);
  else if (city)
    snprintf(text, sizeof(text), "%s", city);
  else if (year[0])
    snprintf(text, sizeof(text), "%s", year);
  else
    snprintf(text, sizeof(text), "?");

  if (strcmp(text, em->last_text) == 0) {
    json_object_put(root);
    return;
  }
  snprintf(em->last_text, sizeof(em->last_text), "%s", text);
  eink_quick_announce(em->display, text);
  json_object_put(root);
}
