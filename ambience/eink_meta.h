#pragma once

#include <stdbool.h>

struct EinkMeta;

// Open the eink display. If flip is true, rotates the display 180 degrees.
// Returns NULL on failure.
struct EinkMeta *eink_meta_init(bool flip);

// Close the eink display and free.
void eink_meta_free(struct EinkMeta *em);

// Parse a slideshow metadata JSON string and render city + year to the eink.
// No-op if em is NULL.
void eink_meta_render(struct EinkMeta *em, const char *meta_json);

// Clear the display to white
void eink_meta_clear(struct EinkMeta *em);
