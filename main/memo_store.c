#include "memo_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WAV_HEADER_SIZE 44U
/* FAT/newlib may expose a signed 32-bit off_t. Keep complete file lengths
 * representable on both the device and the host (about 18.6 hours of audio). */
#define MAX_DATA_BYTES (((uint32_t)INT32_MAX - WAV_HEADER_SIZE) & ~1U)
#define MEMO_NAME_LENGTH 19U

static int fail(int error)
{
    errno = error;
    return -1;
}

static int io_error(void)
{
    return errno ? errno : EIO;
}

static void put16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void make_header(unsigned char header[WAV_HEADER_SIZE], uint32_t bytes)
{
    memset(header, 0, WAV_HEADER_SIZE);
    memcpy(header, "RIFF", 4);
    put32(header + 4, bytes + 36U);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16);
    put16(header + 20, 1); /* PCM */
    put16(header + 22, 1); /* mono */
    put32(header + 24, MEMO_SAMPLE_RATE);
    put32(header + 28, MEMO_SAMPLE_RATE * 2U);
    put16(header + 32, 2);
    put16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put32(header + 40, bytes);
}

/* Count names, including damaged or incomplete files, when reserving IDs.
 * Ignore lookalikes, overflow, zero and noncanonical digit counts. */
static bool parse_name(const char *name, bool allow_partial, uint32_t *id)
{
    size_t length = strlen(name);
    if (length != MEMO_NAME_LENGTH && !(allow_partial && length == MEMO_NAME_LENGTH + 1U)) {
        return false;
    }
    if (memcmp(name, "memo_", 5) != 0) return false;
    if (strcmp(name + 15, ".wav") != 0 &&
        !(allow_partial && strcmp(name + 15, ".part") == 0)) return false;
    uint64_t value = 0;
    for (size_t i = 5; i < 15; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        value = value * 10U + (unsigned)(name[i] - '0');
    }
    if (value == 0 || value > UINT32_MAX) return false;
    *id = (uint32_t)value;
    return true;
}

static int make_path(const memo_store_t *store, uint32_t id, bool partial,
                     char *path, size_t capacity)
{
    if (!store || !store->root[0] || !path || !capacity || !id) return fail(EINVAL);
    int length = snprintf(path, capacity, "%s/memo_%010" PRIu32 ".%s",
                          store->root, id, partial ? "part" : "wav");
    if (length < 0 || (size_t)length >= capacity) return fail(ENAMETOOLONG);
    return 0;
}

static int active_path(const memo_store_t *store, char *path, size_t capacity)
{
    if (!store || !store->root[0] || !path || !capacity) return fail(EINVAL);
    int length = snprintf(path, capacity, "%s/recording.active", store->root);
    if (length < 0 || (size_t)length >= capacity) return fail(ENAMETOOLONG);
    return 0;
}

/* Returns 1 when present, 0 when absent, -1 when malformed/unreadable. */
static int read_active(const memo_store_t *store, uint32_t *id)
{
    char path[MEMO_PATH_MAX];
    if (active_path(store, path, sizeof(path)) != 0) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? 0 : -1;
    char text[32];
    size_t count = fread(text, 1, sizeof(text) - 1U, file);
    int error = ferror(file) ? io_error() : 0;
    if (fclose(file) != 0 && !error) error = io_error();
    if (error) return fail(error);
    text[count] = '\0';
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || value == 0 || value > UINT32_MAX ||
        !(*end == '\0' || (*end == '\n' && end[1] == '\0')))
        return fail(EINVAL);
    if (id) *id = (uint32_t)value;
    return 1;
}

static int clear_active(const memo_store_t *store)
{
    char path[MEMO_PATH_MAX];
    if (active_path(store, path, sizeof(path)) != 0) return -1;
    if (unlink(path) == 0 || errno == ENOENT) return 0;
    return -1;
}

static int write_active(const memo_store_t *store, uint32_t id)
{
    char path[MEMO_PATH_MAX];
    if (active_path(store, path, sizeof(path)) != 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (fd < 0) return -1;
    char text[24];
    int length = snprintf(text, sizeof(text), "%" PRIu32 "\n", id);
    int error = 0;
    if (length <= 0 || (size_t)length >= sizeof(text) ||
        write(fd, text, (size_t)length) != length || fsync(fd) != 0)
        error = io_error();
    if (close(fd) != 0 && !error) error = io_error();
    if (error) {
        unlink(path);
        return fail(error);
    }
    return 0;
}

int memo_store_path(const memo_store_t *store, uint32_t id, char *path, size_t capacity)
{
    return make_path(store, id, false, path, capacity);
}

int memo_store_init(memo_store_t *store, const char *root)
{
    if (!store || !root || !root[0]) return fail(EINVAL);
    size_t length = strlen(root);
    while (length > 1 && root[length - 1] == '/') --length;
    if (length + 1U + MEMO_NAME_LENGTH + 1U >= MEMO_PATH_MAX) return fail(ENAMETOOLONG);
    char normalized_root[MEMO_PATH_MAX];
    memcpy(normalized_root, root, length);
    normalized_root[length] = '\0';
    memset(store, 0, sizeof(*store));
    memcpy(store->root, normalized_root, length + 1U);
    store->next_fail_after_bytes = MEMO_WRITE_FAILURE_DISABLED;
    store->fail_after_bytes = MEMO_WRITE_FAILURE_DISABLED;
    struct stat st;
    if (stat(store->root, &st) != 0) {
        if (errno != ENOENT) return -1;
        if (mkdir(store->root, 0775) != 0 && errno != EEXIST) return -1;
        if (stat(store->root, &st) != 0) return -1;
    }
    if (!S_ISDIR(st.st_mode)) return fail(ENOTDIR);
    return 0;
}

/* Current task filenames reserve their ID even after WAV deletion/discard.
 * Filename exclusion is not a history proof or a trusted device counter. */
static bool parse_task_name(const char *name,uint32_t *id)
{
    if(strlen(name)!=24 || memcmp(name,"sync_r1_",8) ||
       (strcmp(name+18,".job.0") && strcmp(name+18,".job.1")))return false;
    uint64_t value=0;
    for(unsigned i=8;i<18;i++){
        if(name[i]<'0'||name[i]>'9')return false;
        value=value*10U+(unsigned)(name[i]-'0');
    }
    if(!value||value>INT32_MAX)return false;
    *id=(uint32_t)value;return true;
}

static int max_id(const memo_store_t *store, uint32_t *maximum)
{
    DIR *dir = opendir(store->root);
    if (!dir) return -1;
    *maximum = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        uint32_t id;
        if ((parse_name(entry->d_name, true, &id) || parse_task_name(entry->d_name,&id)) && id > *maximum) *maximum = id;
        errno = 0;
    }
    int error = errno;
    if (closedir(dir) != 0 && !error) error = io_error();
    return error ? fail(error) : 0;
}

static int mark_failed(memo_store_t *store, int error)
{
    store->failed = true;
    store->failure_errno = error;
    return fail(error);
}

static int begin_after(memo_store_t *store, uint32_t floor, uint32_t limit)
{
    if (!store || !store->root[0]) return fail(EINVAL);
    if (store->file) return fail(EBUSY);
    /* Do not create or clear a new marker over an unrecovered recording. */
    int pending=read_active(store,NULL);
    if(pending<0)return -1;
    if(pending)return fail(EBUSY);
    uint32_t maximum;
    if (max_id(store, &maximum) != 0) return -1;
    if (floor > maximum) maximum = floor;
    if (maximum >= limit) return fail(EOVERFLOW);
    uint32_t id = maximum + 1U;
    int fd;
    for (;;) {
        if (make_path(store, id, true, store->partial_path, sizeof(store->partial_path)) != 0) return -1;
        fd = open(store->partial_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (fd >= 0) break;
        if (errno != EEXIST) return -1;
        if (id == limit) return fail(EOVERFLOW);
        ++id;
    }
    store->id = id;
    store->data_bytes = 0;
    store->failed = false;
    store->failure_errno = 0;
    store->file = fdopen(fd, "wb");
    if (!store->file) {
        int error = io_error();
        close(fd);
        return mark_failed(store, error);
    }
    store->fail_after_bytes = store->next_fail_after_bytes;
    store->next_fail_after_bytes = MEMO_WRITE_FAILURE_DISABLED;
    unsigned char header[WAV_HEADER_SIZE];
    make_header(header, 0);
    errno = 0;
    if (fwrite(header, 1, sizeof(header), store->file) != sizeof(header) || ferror(store->file)) {
        int error = io_error();
        memo_store_abort(store);
        return mark_failed(store, error);
    }
    /* Make the zero-length header and active marker durable before recording
     * becomes visible. A later boot can then distinguish the one interrupted
     * session from old diagnostic .part files. */
    if (fflush(store->file) != 0 || fsync(fileno(store->file)) != 0 ||
        write_active(store, store->id) != 0) {
        int error = io_error();
        FILE *file = store->file;
        store->file = NULL;
        fclose(file);
        clear_active(store);
        return mark_failed(store, error);
    }
    return 0;
}

int memo_store_max_id(const memo_store_t *store,uint32_t *id) {
    if(!store||!id)return fail(EINVAL);
    return max_id(store,id);
}
int memo_store_begin_reserved(memo_store_t *store,uint32_t id) {
    if(!id || id>INT32_MAX)return fail(EINVAL);
    return begin_after(store,id-1,id);
}

int memo_store_begin(memo_store_t *store)
{
    return begin_after(store, 0, UINT32_MAX);
}
int memo_store_begin_after(memo_store_t *store, uint32_t floor)
{
    return begin_after(store, floor, INT32_MAX);
}

int memo_store_append(memo_store_t *store, const int16_t *samples, size_t sample_count)
{
    if (!store || !store->file || (!samples && sample_count)) return fail(EINVAL);
    if (store->failed) return fail(store->failure_errno);
    if (sample_count > (MAX_DATA_BYTES - store->data_bytes) / 2U) return mark_failed(store, EFBIG);
    size_t bytes = sample_count * 2U;
    if (!bytes) return 0;
    size_t permitted = bytes;
    uint64_t remaining = store->fail_after_bytes > store->data_bytes
        ? store->fail_after_bytes - store->data_bytes : 0;
    if (remaining < permitted) permitted = (size_t)remaining;
    /* ESP32-S3 and supported macOS hosts are little endian. Keep the on-disk
     * representation portable for other host test machines as well. */
    const uint16_t endian_probe = 1;
    if (*(const unsigned char *)&endian_probe == 1) {
        errno = 0;
        size_t written = fwrite(samples, 1, permitted, store->file);
        store->data_bytes += (uint32_t)written;
        if (written != permitted || ferror(store->file)) return mark_failed(store, io_error());
    } else {
        const unsigned char *input = (const unsigned char *)samples;
        unsigned char converted[512];
        size_t position = 0;
        while (position < permitted) {
            size_t block = permitted - position;
            if (block > sizeof(converted)) block = sizeof(converted);
            for (size_t i = 0; i < block; ++i) converted[i] = input[(position + i) ^ 1U];
            errno = 0;
            size_t written = fwrite(converted, 1, block, store->file);
            store->data_bytes += (uint32_t)written;
            if (written != block || ferror(store->file)) return mark_failed(store, io_error());
            position += block;
        }
    }
    if (permitted != bytes) return mark_failed(store, ENOSPC);
    return 0;
}

int memo_store_checkpoint(memo_store_t *store)
{
    if (!store || !store->file) return fail(EINVAL);
    if (store->failed) return fail(store->failure_errno);
    errno = 0;
    if (fflush(store->file) != 0 || fsync(fileno(store->file)) != 0)
        return mark_failed(store, io_error());
    return 0;
}

int memo_store_abort(memo_store_t *store)
{
    if (!store) return fail(EINVAL);
    if (!store->file) return clear_active(store);
    FILE *file = store->file;
    store->file = NULL;
    errno = 0;
    int error = fclose(file) != 0 ? io_error() : 0;
    if (clear_active(store) != 0 && !error) error = io_error();
    if (error) return mark_failed(store, error);
    return 0;
}

static void fill_record(memo_record_t *record, uint32_t id, uint32_t bytes, const char *path)
{
    if (!record) return;
    memset(record, 0, sizeof(*record));
    record->id = id;
    record->data_bytes = bytes;
    record->duration_ms = (uint32_t)((uint64_t)bytes * 1000U / (MEMO_SAMPLE_RATE * 2U));
    memcpy(record->path, path, strlen(path) + 1U);
}

static bool pcm_header_shape_valid(const unsigned char header[WAV_HEADER_SIZE])
{
    return memcmp(header, "RIFF", 4) == 0 &&
           memcmp(header + 8, "WAVEfmt ", 8) == 0 &&
           get32(header + 16) == 16U && get16(header + 20) == 1U &&
           get16(header + 22) == 1U && get32(header + 24) == MEMO_SAMPLE_RATE &&
           get32(header + 28) == MEMO_SAMPLE_RATE * 2U &&
           get16(header + 32) == 2U && get16(header + 34) == 16U &&
           memcmp(header + 36, "data", 4) == 0;
}

int memo_store_inspect_partial(const memo_store_t *store, uint32_t id,
                               memo_record_t *record)
{
    if (!store || !store->root[0] || !id) return fail(EINVAL);
    char path[MEMO_PATH_MAX];
    if (make_path(store, id, true, path, sizeof(path)) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_size < (off_t)(WAV_HEADER_SIZE + 2U) ||
        (uint64_t)st.st_size > (uint64_t)WAV_HEADER_SIZE + MAX_DATA_BYTES)
        return fail(EINVAL);
    uint32_t bytes = (uint32_t)st.st_size - WAV_HEADER_SIZE;
    if (!bytes || bytes % 2U) return fail(EINVAL);
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    unsigned char header[WAV_HEADER_SIZE];
    int error = 0;
    errno = 0;
    if (fread(header, 1, sizeof(header), file) != sizeof(header))
        error = ferror(file) ? io_error() : EINVAL;
    if (!error && !pcm_header_shape_valid(header)) error = EINVAL;
    if (!error) {
        uint32_t declared = get32(header + 40);
        uint32_t riff = get32(header + 4);
        /* A normal in-progress file retains the zero-length opening header. A
         * previous interrupted recovery may already contain the actual size. */
        if (!((declared == 0U && riff == 36U) ||
              (declared == bytes && riff == bytes + 36U))) error = EINVAL;
    }
    errno = 0;
    if (fclose(file) != 0 && !error) error = io_error();
    if (error) return fail(error);
    fill_record(record, id, bytes, path);
    return 0;
}

int memo_store_recover_partial(memo_store_t *store, uint32_t id,
                               memo_record_t *record)
{
    if (!store || store->file) return fail(store && store->file ? EBUSY : EINVAL);
    memo_record_t partial;
    if (memo_store_inspect_partial(store, id, &partial) != 0) return -1;
    char complete[MEMO_PATH_MAX];
    if (memo_store_path(store, id, complete, sizeof(complete)) != 0) return -1;
    struct stat st;
    if (stat(complete, &st) == 0) return fail(EEXIST);
    if (errno != ENOENT) return -1;
    FILE *file = fopen(partial.path, "r+b");
    if (!file) return -1;
    unsigned char header[WAV_HEADER_SIZE];
    make_header(header, partial.data_bytes);
    int error = 0;
    errno = 0;
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), file) != sizeof(header) || ferror(file))
        error = io_error();
    if (!error && fflush(file) != 0) error = io_error();
    if (!error && fsync(fileno(file)) != 0) error = io_error();
    errno = 0;
    if (fclose(file) != 0 && !error) error = io_error();
    if (error) return fail(error);
    if (rename(partial.path, complete) != 0) return fail(io_error());
    fill_record(record, id, partial.data_bytes, complete);
    return 0;
}

int memo_store_recover_pending(memo_store_t *store, memo_record_t *record,
                               uint32_t *pending_id)
{
    if (!store || store->file) return fail(store && store->file ? EBUSY : EINVAL);
    uint32_t id = 0;
    int present = read_active(store, &id);
    if (present <= 0) return present;
    if (pending_id) *pending_id = id;

    char complete[MEMO_PATH_MAX];
    if (memo_store_path(store, id, complete, sizeof(complete)) != 0) return -1;
    memo_record_t validated;
    if (memo_store_validate(complete, &validated) == 0) {
        clear_active(store);
        if (record) *record = validated;
        return 0;
    }
    if (errno != ENOENT && errno != EINVAL) return -1;

    if (memo_store_recover_partial(store, id, record) != 0) {
        int error = errno;
        clear_active(store);
        return fail(error);
    }
    if (clear_active(store) != 0) return -1;
    return 1;
}

int memo_store_pending_id(const memo_store_t *store, uint32_t *pending_id)
{
    if (!store || !pending_id) return fail(EINVAL);
    int present = read_active(store, pending_id);
    if (present < 0 && errno == EINVAL) {
        /* A torn marker cannot identify a recording and would otherwise make
         * every later begin fail with EEXIST.  The .part file, if any, stays
         * isolated for explicit inspection; only unusable metadata is removed. */
        int error = errno;
        if (clear_active(store) != 0) return -1;
        return fail(error);
    }
    return present;
}

int memo_store_discard_partial(memo_store_t *store, uint32_t id)
{
    if (!store || store->file || !id)
        return fail(store && store->file ? EBUSY : EINVAL);
    char path[MEMO_PATH_MAX];
    if (make_path(store, id, true, path, sizeof(path)) != 0) return -1;
    if (unlink(path) != 0) return -1;
    uint32_t active = 0;
    int present = read_active(store, &active);
    if (present < 0) return -1;
    if (present == 1 && active == id && clear_active(store) != 0) return -1;
    return 0;
}

int memo_store_finish(memo_store_t *store, memo_record_t *record)
{
    if (!store || !store->file) return fail(EINVAL);
    int error = store->failed ? store->failure_errno : 0;
    if (!error && store->data_bytes == 0) error = ENODATA;
    if (!error && store->data_bytes % 2U) error = EIO;
    unsigned char header[WAV_HEADER_SIZE];
    make_header(header, store->data_bytes);
    if (!error) {
        errno = 0;
        if (fseek(store->file, 0, SEEK_SET) != 0) error = io_error();
    }
    if (!error) {
        errno = 0;
        if (fwrite(header, 1, sizeof(header), store->file) != sizeof(header) || ferror(store->file)) error = io_error();
    }
    if (!error) {
        errno = 0;
        if (fflush(store->file) != 0) error = io_error();
    }
    if (!error) {
        errno = 0;
        if (fsync(fileno(store->file)) != 0) error = io_error();
    }
    FILE *file = store->file;
    store->file = NULL;
    errno = 0;
    if (fclose(file) != 0 && !error) error = io_error();
    if (error) {
        int saved = error;
        clear_active(store);
        return mark_failed(store, saved);
    }
    char path[MEMO_PATH_MAX];
    if (memo_store_path(store, store->id, path, sizeof(path)) != 0) {
        int saved = errno;
        clear_active(store);
        return mark_failed(store, saved);
    }
    /* The application serializes access. Refuse an unexpected destination,
     * including a malformed file, rather than replacing it with rename. */
    struct stat st;
    if (stat(path, &st) == 0) {
        clear_active(store);
        return mark_failed(store, EEXIST);
    }
    if (errno != ENOENT) {
        int saved = errno;
        clear_active(store);
        return mark_failed(store, saved);
    }
    if (rename(store->partial_path, path) != 0) {
        int saved = io_error();
        clear_active(store);
        return mark_failed(store, saved);
    }
    /* A stale marker after this point is harmless: boot validates the complete
     * destination and removes it without altering the WAV. */
    clear_active(store);
    fill_record(record, store->id, store->data_bytes, path);
    return 0;
}

int memo_store_validate(const char *path, memo_record_t *record)
{
    if (!path || strlen(path) >= MEMO_PATH_MAX) return fail(EINVAL);
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    uint32_t id;
    if (!parse_name(name, false, &id)) return fail(EINVAL);
    struct stat st;
    /* FAT can reject fopen(directory) with EACCES, while host libc accepts it.
     * Identify non-files first so they are consistently excluded from lists. */
    if (stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_size < (off_t)WAV_HEADER_SIZE) return fail(EINVAL);
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    unsigned char header[WAV_HEADER_SIZE];
    int error = 0;
    errno = 0;
    if (fstat(fileno(file), &st) != 0) error = io_error();
    if (!error && (!S_ISREG(st.st_mode) || st.st_size < (off_t)WAV_HEADER_SIZE)) error = EINVAL;
    if (!error) {
        errno = 0;
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) error = ferror(file) ? io_error() : EINVAL;
    }
    uint32_t bytes = 0;
    if (!error) {
        bytes = get32(header + 40);
        if (!bytes || bytes > MAX_DATA_BYTES || bytes % 2U ||
            (uint64_t)st.st_size != (uint64_t)WAV_HEADER_SIZE + bytes ||
            get32(header + 4) != bytes + 36U ||
            !pcm_header_shape_valid(header)) error = EINVAL;
    }
    errno = 0;
    if (fclose(file) != 0 && !error) error = io_error();
    if (error) return fail(error);
    fill_record(record, id, bytes, path);
    return 0;
}

int memo_store_list(const memo_store_t *store, uint32_t before_id,
                    memo_record_t *records, size_t capacity,
                    size_t *count, size_t *total)
{
    if (count) *count = 0;
    if (total) *total = 0;
    if (!store || !store->root[0] || (capacity && !records)) return fail(EINVAL);
    DIR *dir = opendir(store->root);
    if (!dir) return -1;
    size_t used = 0, all = 0;
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            error = errno;
            if (error) fprintf(stderr, "STORE_READDIR path=%s valid=%zu errno=%d\n",
                               store->root, all, error);
            break;
        }
        uint32_t id;
        if (!parse_name(entry->d_name, false, &id)) continue;
        char path[MEMO_PATH_MAX];
        if (memo_store_path(store, id, path, sizeof(path)) != 0) {
            error = errno;
            break;
        }
        memo_record_t record;
        if (memo_store_validate(path, &record) != 0) {
            /* Malformed recordings are isolated by omission. Actual device or
             * media errors must reach the caller rather than look like no memos. */
            if (errno == EINVAL || errno == ENOENT || errno == EISDIR) continue;
            error = errno;
            fprintf(stderr, "STORE_VALIDATE path=%s errno=%d (%s)\n",
                    path, error, strerror(error));
            break;
        }
        ++all;
        if (!capacity || (before_id && id >= before_id)) continue;
        size_t position = 0;
        while (position < used && records[position].id > id) ++position;
        if (position >= capacity) continue;
        if (used < capacity) ++used;
        for (size_t i = used - 1; i > position; --i) records[i] = records[i - 1];
        records[position] = record;
    }
    errno = 0;
    if (closedir(dir) != 0 && !error) error = io_error();
    if (error) return fail(error);
    if (count) *count = used;
    if (total) *total = all;
    return 0;
}

static int descending_record_id(const void *left, const void *right)
{
    const memo_record_t *a = left, *b = right;
    return a->id < b->id ? 1 : a->id > b->id ? -1 : 0;
}

int memo_store_index(const memo_store_t *store, memo_record_t **records, size_t *count)
{
    if (records) *records = NULL;
    if (count) *count = 0;
    if (!store || !store->root[0] || !records || !count) return fail(EINVAL);
    DIR *dir = opendir(store->root);
    if (!dir) return -1;
    memo_record_t *items = NULL;
    size_t used = 0, capacity = 0;
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            error = errno;
            if (error) fprintf(stderr, "STORE_READDIR path=%s valid=%zu errno=%d\n",
                               store->root, used, error);
            break;
        }
        uint32_t id;
        if (!parse_name(entry->d_name, false, &id)) continue;
        char path[MEMO_PATH_MAX];
        if (memo_store_path(store, id, path, sizeof(path)) != 0) {
            error = errno;
            break;
        }
        memo_record_t record;
        if (memo_store_validate(path, &record) != 0) {
            if (errno == EINVAL || errno == ENOENT || errno == EISDIR) continue;
            error = errno;
            fprintf(stderr, "STORE_VALIDATE path=%s errno=%d (%s)\n",
                    path, error, strerror(error));
            break;
        }
        if (used == capacity) {
            size_t next = capacity ? capacity * 2U : 16U;
            if (next < capacity || next > SIZE_MAX / sizeof(*items)) {
                error = EOVERFLOW;
                break;
            }
            memo_record_t *grown = realloc(items, next * sizeof(*items));
            if (!grown) {
                error = errno ? errno : ENOMEM;
                break;
            }
            items = grown;
            capacity = next;
        }
        items[used++] = record;
    }
    errno = 0;
    if (closedir(dir) != 0 && !error) error = io_error();
    if (error) {
        free(items);
        return fail(error);
    }
    if (used > 1) qsort(items, used, sizeof(*items), descending_record_id);
    *records = items;
    *count = used;
    return 0;
}

int memo_store_inject_write_failure(memo_store_t *store, uint64_t after_bytes)
{
    if (!store || !store->root[0]) return fail(EINVAL);
    if (store->file) return fail(EBUSY);
    store->next_fail_after_bytes = after_bytes;
    return 0;
}
