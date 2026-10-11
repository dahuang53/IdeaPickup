#include "memo_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s (errno=%d: %s)\n", \
                __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        exit(1); \
    } \
} while (0)
#define EXPECT_ERROR(expression, expected) do { \
    errno = 0; CHECK((expression) == -1); CHECK(errno == (expected)); \
} while (0)

static int checks;

static void done(const char *name)
{
    ++checks;
    printf("PASS %s\n", name);
}

static void join_path(char *out, size_t capacity, const char *dir, const char *name)
{
    int length = snprintf(out, capacity, "%s/%s", dir, name);
    CHECK(length >= 0 && (size_t)length < capacity);
}

static memo_store_t make_store(const char *parent, const char *name)
{
    char root[MEMO_PATH_MAX];
    join_path(root, sizeof(root), parent, name);
    memo_store_t store;
    CHECK(memo_store_init(&store, root) == 0);
    return store;
}

static void write_bytes(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    CHECK(file);
    CHECK(fwrite(data, 1, size, file) == size);
    CHECK(fclose(file) == 0);
}

static void expect_bytes(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "rb");
    CHECK(file);
    const unsigned char *expected = data;
    for (size_t i = 0; i < size; ++i) CHECK(fgetc(file) == expected[i]);
    CHECK(fgetc(file) == EOF);
    CHECK(fclose(file) == 0);
}

static uint32_t le32(const unsigned char *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void verify_header(const memo_record_t *record, uint32_t samples)
{
    unsigned char header[44];
    FILE *file = fopen(record->path, "rb");
    CHECK(file);
    CHECK(fread(header, 1, sizeof(header), file) == sizeof(header));
    CHECK(memcmp(header, "RIFF", 4) == 0);
    CHECK(le32(header + 4) == samples * 2U + 36U);
    CHECK(memcmp(header + 8, "WAVEfmt ", 8) == 0);
    CHECK(le32(header + 16) == 16);
    CHECK(header[20] == 1 && header[21] == 0);
    CHECK(header[22] == 1 && header[23] == 0);
    CHECK(le32(header + 24) == 16000);
    CHECK(le32(header + 28) == 32000);
    CHECK(header[32] == 2 && header[33] == 0);
    CHECK(header[34] == 16 && header[35] == 0);
    CHECK(memcmp(header + 36, "data", 4) == 0);
    CHECK(le32(header + 40) == samples * 2U);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    CHECK(ftell(file) == (long)(44U + samples * 2U));
    CHECK(fclose(file) == 0);
    CHECK(record->data_bytes == samples * 2U);
    CHECK(record->duration_ms == (uint32_t)((uint64_t)samples * 1000U / 16000U));
    memo_record_t validated;
    CHECK(memo_store_validate(record->path, &validated) == 0);
    CHECK(validated.id == record->id);
    CHECK(validated.data_bytes == record->data_bytes);
}

static memo_record_t record_samples(memo_store_t *store, uint32_t count)
{
    int16_t block[1600];
    for (size_t i = 0; i < 1600; ++i) block[i] = (int16_t)((int)i * 17 - 13000);
    CHECK(memo_store_begin(store) == 0);
    uint32_t remaining = count;
    while (remaining) {
        size_t batch = remaining < 1600U ? remaining : 1600U;
        CHECK(memo_store_append(store, block, batch) == 0);
        remaining -= (uint32_t)batch;
    }
    memo_record_t record;
    CHECK(memo_store_finish(store, &record) == 0);
    verify_header(&record, count);
    return record;
}

static void test_ten_and_restart(const char *parent)
{
    memo_store_t store = make_store(parent, "ten");
    const uint32_t lengths[10] = {1, 16, 160, 12345, 16000, 32001, 31999, 16001, 234567, 120U * 16000U};
    for (uint32_t i = 0; i < 10; ++i) {
        if (i == 5) {
            char root[MEMO_PATH_MAX];
            strcpy(root, store.root);
            memset(&store, 0xA5, sizeof(store));
            CHECK(memo_store_init(&store, root) == 0);
        }
        memo_record_t record = record_samples(&store, lengths[i]);
        CHECK(record.id == i + 1U);
    }
    memo_record_t records[10];
    size_t count = 99, total = 99;
    CHECK(memo_store_list(&store, 0, records, 10, &count, &total) == 0);
    CHECK(count == 10 && total == 10);
    memo_record_t *index = NULL;
    size_t indexed = 0;
    CHECK(memo_store_index(&store, &index, &indexed) == 0);
    CHECK(indexed == 10);
    for (size_t i = 0; i < indexed; ++i) CHECK(index[i].id == 10 - i);
    free(index);
    for (size_t i = 0; i < count; ++i) {
        CHECK(records[i].id == 10 - i);
        verify_header(&records[i], lengths[9 - i]);
    }
    CHECK(records[0].duration_ms == 120000);
    uint32_t before = 0, expected = 10;
    do {
        CHECK(memo_store_list(&store, before, records, 3, &count, &total) == 0);
        CHECK(total == 10);
        for (size_t i = 0; i < count; ++i) CHECK(records[i].id == expected--);
        if (count) before = records[count - 1].id;
    } while (count);
    CHECK(expected == 0);
    CHECK(memo_store_list(&store, 0, NULL, 0, &count, &total) == 0);
    CHECK(count == 0 && total == 10);
    done("ten unique recordings, restart, descending paginated list, 120-second streaming WAV and exact headers");
}

static void test_short_and_abort(const char *parent)
{
    memo_store_t store = make_store(parent, "short");
    CHECK(memo_store_begin(&store) == 0);
    EXPECT_ERROR(memo_store_begin(&store), EBUSY);
    EXPECT_ERROR(memo_store_inject_write_failure(&store, 0), EBUSY);
    CHECK(memo_store_append(&store, NULL, 0) == 0);
    EXPECT_ERROR(memo_store_finish(&store, NULL), ENODATA);
    CHECK(access(store.partial_path, F_OK) == 0);
    CHECK(memo_store_abort(&store) == 0);
    CHECK(memo_store_begin(&store) == 0);
    CHECK(store.id == 2);
    int16_t sample = (int16_t)0x8123;
    CHECK(memo_store_append(&store, &sample, 1) == 0);
    CHECK(memo_store_abort(&store) == 0);
    EXPECT_ERROR(memo_store_validate(store.partial_path, NULL), EINVAL);
    CHECK(memo_store_begin(&store) == 0);
    CHECK(store.id == 3);
    CHECK(memo_store_append(&store, &sample, 1) == 0);
    memo_record_t record;
    CHECK(memo_store_finish(&store, &record) == 0);
    verify_header(&record, 1);
    FILE *file = fopen(record.path, "rb");
    CHECK(file);
    CHECK(fseek(file, 44, SEEK_SET) == 0);
    CHECK(fgetc(file) == 0x23 && fgetc(file) == 0x81);
    CHECK(fclose(file) == 0);
    size_t count, total;
    CHECK(memo_store_list(&store, 0, &record, 1, &count, &total) == 0);
    CHECK(count == 1 && total == 1 && record.id == 3);
    done("zero-sample rejection, one-sample WAV, endian encoding, busy guards and interrupted .part isolation");
}

static void test_explicit_partial_recovery(const char *parent)
{
    memo_store_t store = make_store(parent, "recover_partial");
    int16_t samples[16000];
    for (size_t i = 0; i < 16000; ++i) samples[i] = (int16_t)(i * 13U);
    CHECK(memo_store_begin(&store) == 0);
    CHECK(memo_store_append(&store, samples, 16000) == 0);
    CHECK(memo_store_abort(&store) == 0);
    memo_record_t partial;
    CHECK(memo_store_inspect_partial(&store, 1, &partial) == 0);
    CHECK(partial.id == 1 && partial.data_bytes == 32000 && partial.duration_ms == 1000);
    char partial_path[MEMO_PATH_MAX];
    strcpy(partial_path, partial.path);
    CHECK(access(partial_path, F_OK) == 0);
    memo_record_t recovered;
    CHECK(memo_store_recover_partial(&store, 1, &recovered) == 0);
    CHECK(access(partial_path, F_OK) != 0 && errno == ENOENT);
    verify_header(&recovered, 16000);
    EXPECT_ERROR(memo_store_recover_partial(&store, 1, NULL), ENOENT);

    CHECK(memo_store_begin(&store) == 0);
    CHECK(memo_store_append(&store, samples, 8) == 0);
    CHECK(memo_store_abort(&store) == 0);
    strcpy(partial_path, store.partial_path);
    char complete[MEMO_PATH_MAX];
    CHECK(memo_store_path(&store, 2, complete, sizeof(complete)) == 0);
    write_bytes(complete, "reserved", 8);
    EXPECT_ERROR(memo_store_recover_partial(&store, 2, NULL), EEXIST);
    CHECK(access(partial_path, F_OK) == 0);
    expect_bytes(complete, "reserved", 8);

    CHECK(memo_store_begin(&store) == 0);
    CHECK(memo_store_append(&store, samples, 8) == 0);
    CHECK(memo_store_abort(&store) == 0);
    FILE *file = fopen(store.partial_path, "r+b");
    CHECK(file && fputc('X', file) != EOF && fclose(file) == 0);
    EXPECT_ERROR(memo_store_inspect_partial(&store, 3, NULL), EINVAL);
    EXPECT_ERROR(memo_store_recover_partial(&store, 3, NULL), EINVAL);

    CHECK(memo_store_begin(&store) == 0);
    CHECK(memo_store_append(&store, samples, 8) == 0);
    EXPECT_ERROR(memo_store_recover_partial(&store, 4, NULL), EBUSY);
    CHECK(memo_store_abort(&store) == 0);
    done("explicit interrupted recording recovery, exact WAV repair and no overwrite");
}

static void crash_close(memo_store_t *store)
{
    CHECK(store->file);
    CHECK(fflush(store->file) == 0);
    CHECK(fsync(fileno(store->file)) == 0);
    CHECK(fclose(store->file) == 0);
    store->file = NULL; /* Simulate process/power loss: do not run abort/finish. */
}

static void test_marked_boot_recovery_and_discard(const char *parent)
{
    memo_store_t store = make_store(parent, "boot_recovery");
    int16_t samples[1600];
    for (size_t i = 0; i < 1600; ++i) samples[i] = (int16_t)(i * 7U);

    CHECK(memo_store_begin(&store) == 0);
    CHECK(memo_store_append(&store, samples, 1600) == 0);
    CHECK(memo_store_checkpoint(&store) == 0);
    char partial[MEMO_PATH_MAX];
    strcpy(partial, store.partial_path);
    crash_close(&store);

    char root[MEMO_PATH_MAX];
    strcpy(root, store.root);
    CHECK(memo_store_init(&store, root) == 0);
    memo_record_t recovered;
    uint32_t pending = 0;
    EXPECT_ERROR(memo_store_begin_after(&store,500),EBUSY);
    CHECK(memo_store_pending_id(&store,&pending)==1 && pending==1);
    CHECK(access(partial,F_OK)==0);
    CHECK(memo_store_recover_pending(&store, &recovered, &pending) == 1);
    CHECK(pending == 1 && recovered.id == 1);
    verify_header(&recovered, 1600);
    CHECK(access(partial, F_OK) == -1 && errno == ENOENT);
    CHECK(memo_store_recover_pending(&store, NULL, NULL) == 0);

    /* A crash after rename but before marker deletion must preserve the
     * completed destination and merely clear its stale marker. */
    char active[MEMO_PATH_MAX];
    join_path(active, sizeof(active), store.root, "recording.active");
    write_bytes(active, "1\n", 2);
    memset(&recovered, 0, sizeof(recovered));
    CHECK(memo_store_recover_pending(&store, &recovered, &pending) == 0);
    CHECK(recovered.id == 1);
    verify_header(&recovered, 1600);
    CHECK(access(active, F_OK) == -1 && errno == ENOENT);

    CHECK(memo_store_begin(&store) == 0);
    CHECK(memo_store_append(&store, samples, 8) == 0);
    strcpy(partial, store.partial_path);
    crash_close(&store);
    FILE *file = fopen(partial, "r+b");
    CHECK(file && fputc('X', file) != EOF && fclose(file) == 0);
    EXPECT_ERROR(memo_store_recover_pending(&store, NULL, &pending), EINVAL);
    CHECK(pending == 2);
    CHECK(access(partial, F_OK) == 0);
    CHECK(access(active, F_OK) == -1 && errno == ENOENT);
    CHECK(memo_store_discard_partial(&store, 2) == 0);
    CHECK(access(partial, F_OK) == -1 && errno == ENOENT);
    EXPECT_ERROR(memo_store_discard_partial(&store, 2), ENOENT);

    /* Torn recovery metadata must not strand the store in EEXIST forever.
     * Unknown partial data is retained because it cannot be attributed safely. */
    write_bytes(active, "not-an-id\n", 10);
    EXPECT_ERROR(memo_store_pending_id(&store, &pending), EINVAL);
    CHECK(access(active, F_OK) == -1 && errno == ENOENT);
    CHECK(memo_store_begin(&store) == 0);
    CHECK(memo_store_abort(&store) == 0);
    verify_header(&recovered, 1600);
    done("durable active marker, boot recovery, stale-marker cleanup and explicit partial discard");
}

static void test_injected_failure(const char *parent)
{
    memo_store_t store = make_store(parent, "failure");
    memo_record_t original = record_samples(&store, 2);
    unsigned char old_bytes[48];
    FILE *file = fopen(original.path, "rb");
    CHECK(file);
    CHECK(fread(old_bytes, 1, sizeof(old_bytes), file) == sizeof(old_bytes));
    CHECK(fclose(file) == 0);
    const uint64_t thresholds[] = {0, 1, 128, 200};
    int16_t samples[100] = {0};
    for (size_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); ++i) {
        CHECK(memo_store_inject_write_failure(&store, thresholds[i]) == 0);
        CHECK(memo_store_begin(&store) == 0);
        int result = memo_store_append(&store, samples, 100);
        if (thresholds[i] == 200) {
            CHECK(result == 0);
            EXPECT_ERROR(memo_store_append(&store, samples, 1), ENOSPC);
        } else {
            CHECK(result == -1 && errno == ENOSPC);
        }
        CHECK(store.data_bytes == thresholds[i]);
        EXPECT_ERROR(memo_store_append(&store, samples, 1), ENOSPC);
        memo_record_t sentinel;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        EXPECT_ERROR(memo_store_finish(&store, &sentinel), ENOSPC);
        CHECK(sentinel.id == 0xA5A5A5A5U);
        char final_path[MEMO_PATH_MAX];
        CHECK(memo_store_path(&store, store.id, final_path, sizeof(final_path)) == 0);
        CHECK(access(final_path, F_OK) == -1 && errno == ENOENT);
        CHECK(access(store.partial_path, F_OK) == 0);
        expect_bytes(original.path, old_bytes, sizeof(old_bytes));
        memo_record_t recovered = record_samples(&store, 10);
        CHECK(recovered.id == (uint32_t)(3 + i * 2));
    }
    size_t total;
    CHECK(memo_store_list(&store, 0, NULL, 0, NULL, &total) == 0);
    CHECK(total == 5);
    done("non-destructive next-recording write failures at zero, odd, partial and exact byte boundaries; recovery");
}

static void test_finalize_errors(const char *parent)
{
    memo_store_t store = make_store(parent, "finalize");
    const int16_t sample = 100;
    CHECK(memo_store_begin(&store) == 0);
    CHECK(memo_store_append(&store, &sample, 1) == 0);
    CHECK(fflush(store.file) == 0);
    /* Force real descriptor I/O failure, covering unsuccessful close/finalize. */
    CHECK(close(fileno(store.file)) == 0);
    EXPECT_ERROR(memo_store_finish(&store, NULL), EBADF);
    CHECK(access(store.partial_path, F_OK) == 0);
    char final_path[MEMO_PATH_MAX];
    CHECK(memo_store_path(&store, 1, final_path, sizeof(final_path)) == 0);
    CHECK(access(final_path, F_OK) == -1 && errno == ENOENT);
    CHECK(memo_store_begin(&store) == 0);
    CHECK(memo_store_append(&store, &sample, 1) == 0);
    CHECK(memo_store_path(&store, store.id, final_path, sizeof(final_path)) == 0);
    const char original[] = "preserve unexpected pre-existing destination";
    write_bytes(final_path, original, sizeof(original));
    EXPECT_ERROR(memo_store_finish(&store, NULL), EEXIST);
    expect_bytes(final_path, original, sizeof(original));
    CHECK(access(store.partial_path, F_OK) == 0);
    /* A complete header in a .part does not make it appear in the inbox. */
    size_t total;
    CHECK(memo_store_list(&store, 0, NULL, 0, NULL, &total) == 0);
    CHECK(total == 0);
    memo_record_t recovered = record_samples(&store, 10);
    CHECK(recovered.id == 3);
    done("real finalization I/O failure, no success output, destination preservation and recovery");
}

static void test_corruption_and_names(const char *parent)
{
    memo_store_t store = make_store(parent, "corruption");
    memo_record_t valid = record_samples(&store, 2);
    unsigned char bytes[49];
    FILE *file = fopen(valid.path, "rb");
    CHECK(file);
    CHECK(fread(bytes, 1, 48, file) == 48);
    CHECK(fclose(file) == 0);
    const size_t offsets[] = {0, 4, 8, 12, 16, 20, 22, 24, 28, 32, 34, 36, 40};
    char path[MEMO_PATH_MAX];
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        CHECK(memo_store_path(&store, (uint32_t)i + 2, path, sizeof(path)) == 0);
        bytes[offsets[i]] ^= 0xFF;
        write_bytes(path, bytes, 48);
        bytes[offsets[i]] ^= 0xFF;
        EXPECT_ERROR(memo_store_validate(path, NULL), EINVAL);
    }
    CHECK(memo_store_path(&store, 20, path, sizeof(path)) == 0);
    write_bytes(path, bytes, 47);
    EXPECT_ERROR(memo_store_validate(path, NULL), EINVAL);
    CHECK(memo_store_path(&store, 21, path, sizeof(path)) == 0);
    bytes[48] = 0;
    write_bytes(path, bytes, 49);
    EXPECT_ERROR(memo_store_validate(path, NULL), EINVAL);
    CHECK(memo_store_path(&store, 22, path, sizeof(path)) == 0);
    CHECK(mkdir(path, 0775) == 0);
    EXPECT_ERROR(memo_store_validate(path, NULL), EINVAL);
    const char *lookalikes[] = {"memo_0000000000.wav", "memo_4294967296.wav",
        "memo_99999999999.wav", "memo_000000009.wav", "memo_00000000x9.wav",
        "memo_0000000099.wav.bak", "memo_0000000099.WAV", "other_0000000999.wav"};
    for (size_t i = 0; i < sizeof(lookalikes) / sizeof(lookalikes[0]); ++i) {
        join_path(path, sizeof(path), store.root, lookalikes[i]);
        write_bytes(path, bytes, 48);
        EXPECT_ERROR(memo_store_validate(path, NULL), EINVAL);
    }
    join_path(path, sizeof(path), store.root, "memo_0000000030.part");
    write_bytes(path, bytes, 48);
    memo_record_t records[10];
    size_t count, total;
    CHECK(memo_store_list(&store, 0, records, 10, &count, &total) == 0);
    CHECK(count == 1 && total == 1 && records[0].id == 1);
    memo_record_t next = record_samples(&store, 2);
    CHECK(next.id == 31);
    done("strict names and WAV layout; damaged, truncated, trailing, directory and .part entries excluded; IDs reserved");
}

static void test_id_and_size_boundaries(const char *parent)
{
    memo_store_t store = make_store(parent, "boundaries");
    char path[MEMO_PATH_MAX];
    CHECK(memo_store_path(&store, UINT32_MAX - 1, path, sizeof(path)) == 0);
    write_bytes(path, "reserved", 8);
    memo_record_t record = record_samples(&store, 1);
    CHECK(record.id == UINT32_MAX);
    EXPECT_ERROR(memo_store_begin(&store), EOVERFLOW);
    char root[MEMO_PATH_MAX];
    strcpy(root, store.root);
    CHECK(memo_store_init(&store, root) == 0);
    EXPECT_ERROR(memo_store_begin(&store), EOVERFLOW);
    store = make_store(parent, "size_limit");
    CHECK(memo_store_begin(&store) == 0);
    const int16_t sample = 0;
    /* Preflight must reject this without reading an enormous caller buffer. */
    EXPECT_ERROR(memo_store_append(&store, &sample, (size_t)UINT32_MAX / 2U), EFBIG);
    EXPECT_ERROR(memo_store_finish(&store, NULL), EFBIG);
    EXPECT_ERROR(memo_store_path(&store, 0, path, sizeof(path)), EINVAL);
    EXPECT_ERROR(memo_store_path(&store, 1, path, 2), ENAMETOOLONG);
    done("maximum ID never wraps or overwrites; WAV size preflight, short output path and invalid ID guards");
}

static void test_shared_counter(const char *parent)
{
    memo_store_t store=make_store(parent,"shared_counter");
    CHECK(memo_store_begin_after(&store,99998)==0);
    CHECK(store.id==99999);
    const int16_t sample=123;
    CHECK(memo_store_append(&store,&sample,1)==0);
    memo_record_t record;
    CHECK(memo_store_finish(&store,&record)==0);
    CHECK(record.id==99999);
    CHECK(memo_store_begin_after(&store,99990)==0);
    CHECK(store.id==100000);
    CHECK(memo_store_abort(&store)==0);
    EXPECT_ERROR(memo_store_begin_after(&store,INT32_MAX),EOVERFLOW);
    EXPECT_ERROR(memo_store_begin_after(&store,UINT32_MAX),EOVERFLOW);
    done("shared visible counter respects SD maximum, cardless floor and namespace boundary");
}

static void test_reserved_and_fallback(const char *parent)
{
    memo_store_t store=make_store(parent,"reserved");
    CHECK(memo_store_begin_reserved(&store,42)==0 && store.id==42);
    const int16_t sample=321;
    CHECK(memo_store_append(&store,&sample,1)==0);
    memo_record_t record;
    CHECK(memo_store_finish(&store,&record)==0);
    uint32_t maximum=0;
    CHECK(memo_store_max_id(&store,&maximum)==0 && maximum==42);
    EXPECT_ERROR(memo_store_begin_reserved(&store,42),EOVERFLOW);
    CHECK(memo_store_validate(record.path,NULL)==0);
    CHECK(memo_store_begin_after(&store,maximum)==0 && store.id==43);
    CHECK(memo_store_append(&store,&sample,1)==0);
    CHECK(memo_store_finish(&store,NULL)==0);
    CHECK(memo_store_max_id(&store,&maximum)==0 && maximum==43);
    CHECK(memo_store_begin_reserved(&store,100)==0 && store.id==100);
    CHECK(memo_store_abort(&store)==0);
    CHECK(memo_store_max_id(&store,&maximum)==0 && maximum==100);
    done("exact device reservation never substitutes/overwrites; allocator-fault local fallback remains exclusive");
}

static void test_init_and_removed_media(const char *parent)
{
    memo_store_t store;
    EXPECT_ERROR(memo_store_init(&store, ""), EINVAL);
    char long_root[MEMO_PATH_MAX + 2];
    memset(long_root, 'a', sizeof(long_root) - 1);
    long_root[sizeof(long_root) - 1] = '\0';
    EXPECT_ERROR(memo_store_init(&store, long_root), ENAMETOOLONG);
    char path[MEMO_PATH_MAX];
    join_path(path, sizeof(path), parent, "plain_file");
    write_bytes(path, "x", 1);
    EXPECT_ERROR(memo_store_init(&store, path), ENOTDIR);
    join_path(path, sizeof(path), parent, "missing_parent/recordings");
    EXPECT_ERROR(memo_store_init(&store, path), ENOENT);
    store = make_store(parent, "removed");
    CHECK(memo_store_init(&store, store.root) == 0);
    CHECK(rmdir(store.root) == 0);
    EXPECT_ERROR(memo_store_begin(&store), ENOENT);
    size_t total = 100, count = 100;
    EXPECT_ERROR(memo_store_list(&store, 0, NULL, 0, &count, &total), ENOENT);
    CHECK(total == 0 && count == 0);
    done("invalid root, missing mount parent and removed-media errors never silently recreate a mount");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    struct stat st;
    CHECK(stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode));
    test_ten_and_restart(argv[1]);
    test_short_and_abort(argv[1]);
    test_explicit_partial_recovery(argv[1]);
    test_marked_boot_recovery_and_discard(argv[1]);
    test_injected_failure(argv[1]);
    test_finalize_errors(argv[1]);
    test_corruption_and_names(argv[1]);
    test_id_and_size_boundaries(argv[1]);
    test_init_and_removed_media(argv[1]);
    test_shared_counter(argv[1]);
    test_reserved_and_fallback(argv[1]);
    printf("HOST_STORAGE_PASS groups=%d (synthetic host fixtures; no hardware acceptance claimed)\n", checks);
    return 0;
}
