#pragma once
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The existing relay's edge control-word/whitespace cleanup, streamed with
 * fixed workspace. Input is validated UTF-8 upstream. NULL sink counts bytes;
 * SIZE_MAX reports a sink failure. It never allocates or edits its input. */
typedef bool (*memo_text_sink_t)(void *,const char *,size_t);
size_t memo_transcript_clean(const char *,size_t,memo_text_sink_t,void *);
#ifdef __cplusplus
}
#endif
