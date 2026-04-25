#pragma once

#include <stdbool.h>
#include <stddef.h>

void device_id_init(void);

// Friendly name as the user typed it.
const char *device_id_name(void);

// Desired hostname slug derived from the friendly name (or the default
// hostname if the name slugs to empty). What we *want* to be on mDNS.
const char *device_id_slug(void);

// MAC-suffixed default ("wakelight-XXXX"). Always unique per device.
const char *device_id_default_hostname(void);

// The hostname actually published on mDNS — same as slug if uncontested,
// or slug-2, slug-3, etc. on conflict. Set by the mDNS layer after probing.
const char *device_id_chosen_hostname(void);
void device_id_set_chosen_hostname(const char *host);

// Whether this device currently holds the shared "wakelight" delegate.
bool device_id_holds_wakelight(void);
void device_id_set_holds_wakelight(bool v);

// Persist a new friendly name. Empty/oversize names are rejected.
bool device_id_set_name(const char *name);

// Compute the slug for an arbitrary name without touching persisted state.
// Writes a lowercase [a-z0-9-] string into `out` (0-terminated).
void device_id_slug_for(const char *name, char *out, size_t out_cap);
