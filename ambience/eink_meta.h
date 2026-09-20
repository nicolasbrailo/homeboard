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

// Signals the ambience service became inactive by clearing the eink and
// displaying host info. No-op if em is NULL.
void eink_meta_set_inactive(struct EinkMeta *em);

// Signals the fallback image is showing because no photo could be displayed.
// `reason` is what photo-provider said about it, shown instead of the generic
// line; pass NULL or "" when there's nothing more specific to say.
// No-op if em is NULL.
void eink_meta_set_no_photo(struct EinkMeta *em, const char *reason);

// Clear the display to white
void eink_meta_clear(struct EinkMeta *em);
