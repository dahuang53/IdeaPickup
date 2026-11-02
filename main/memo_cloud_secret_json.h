#pragma once
#include <stddef.h>
namespace memo_cloud {
// Internal USB parser bridge. Reuses the isolated pinned SDK parser; callback
// may inspect its private cJSON tree only until it returns. Entire 16KiB arena
// is wiped on every exit, including malformed input/partial allocation.
int secret_configuration_json(const char *,size_t,int rejected,int (*visit)(void *,void *),void *);
}
