#ifndef MEMO_STORE_H
#define MEMO_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMO_SAMPLE_RATE 16000U
#define MEMO_PATH_MAX 256U
#define MEMO_WRITE_FAILURE_DISABLED UINT64_MAX

typedef struct {
    uint32_t id;
    uint32_t data_bytes;
    uint32_t duration_ms;
    char path[MEMO_PATH_MAX];
} memo_record_t;

/* A store is owned by the audio worker; callers must serialize access. */
typedef struct {
    char root[MEMO_PATH_MAX];
    FILE *file;
    uint32_t id;
    uint32_t data_bytes;
    uint64_t fail_after_bytes;
    uint64_t next_fail_after_bytes;
    bool failed;
    int failure_errno;
    char partial_path[MEMO_PATH_MAX];
} memo_store_t;

/* All int APIs return 0 on success, -1 with errno on failure. No SD formatting.
 * init creates root only if its parent exists. Caller mounts SD first and checks
 * available capacity before begin. root must fit with a 20-character filename.
 * The complete file is capped below 2 GiB for signed 32-bit FAT/newlib offsets.
 */
int memo_store_init(memo_store_t *store, const char *root);
int memo_store_begin(memo_store_t *store);
/* Exclusive card-local fallback after visible names; not a device/global ID authority. */
int memo_store_begin_after(memo_store_t *store, uint32_t floor);
/* Device allocator reserves first; exact exclusive filename, no higher-ID substitution. */
int memo_store_begin_reserved(memo_store_t *store, uint32_t id);
int memo_store_max_id(const memo_store_t *store, uint32_t *id);
int memo_store_append(memo_store_t *store, const int16_t *samples, size_t sample_count);
/* Persist current PCM length/allocation while keeping the zero-length
 * provisional header. Used sparingly so boot recovery has a bounded loss. */
int memo_store_checkpoint(memo_store_t *store);
int memo_store_finish(memo_store_t *store, memo_record_t *record);
/* Close an active recording, clear its automatic-recovery marker and retain
 * its .part file excluded from the list for explicit diagnostics. */
int memo_store_abort(memo_store_t *store);

/* Descending ID order; before_id=0 starts at newest. Pass the last returned ID
 * as before_id for the next page. total counts every valid completed recording.
 * count and total may be NULL. capacity=0 is permitted for a count-only scan.
 */
int memo_store_list(const memo_store_t *store, uint32_t before_id,
                    memo_record_t *records, size_t capacity,
                    size_t *count, size_t *total);
/* Read and validate the directory once, returning every completed recording in
 * descending ID order.  The caller owns *records and frees it with free(). */
int memo_store_index(const memo_store_t *store, memo_record_t **records, size_t *count);
/* Validates this application's strict 44-byte PCM WAV layout, exact length and
 * filename. Unfinished .part files are never returned as completed records. */
int memo_store_validate(const char *path, memo_record_t *record);
int memo_store_path(const memo_store_t *store, uint32_t id, char *path, size_t capacity);
/* Inspect or explicitly recover PCM left in one .part file after unexpected
 * power loss. Recovery rewrites the WAV lengths, fsyncs, and refuses to
 * replace an existing completed file. */
int memo_store_inspect_partial(const memo_store_t *store, uint32_t id,
                               memo_record_t *record);
int memo_store_recover_partial(memo_store_t *store, uint32_t id,
                               memo_record_t *record);
/* Recover only the recording named by the durable active marker. Returns 1
 * when a WAV was recovered, 0 when no recovery was needed, and -1 when the
 * marker/partial was invalid (the incomplete file remains isolated). */
int memo_store_recover_pending(memo_store_t *store, memo_record_t *record,
                               uint32_t *pending_id);
/* Read the durable marker without modifying either marker or partial. Returns
 * 1 when present, 0 when absent, and -1 when malformed/unreadable. */
/* Read the durable recovery marker. A malformed/torn marker is removed so it
 * cannot permanently block future recordings; any .part file is preserved. */
int memo_store_pending_id(const memo_store_t *store, uint32_t *pending_id);
/* Explicitly remove one isolated partial; completed WAVs are never touched. */
int memo_store_discard_partial(memo_store_t *store, uint32_t id);
/* Non-destructive test hook: fail the NEXT recording once this many PCM bytes
 * have been written. It resets to disabled when begin successfully opens a file.
 * Existing completed files are never changed. Do not call during recording. */
int memo_store_inject_write_failure(memo_store_t *store, uint64_t after_bytes);

#ifdef __cplusplus
}
#endif

#endif
