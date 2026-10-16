#ifndef MEMO_NETWORK_CONFIG_H
#define MEMO_NETWORK_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MEMO_CONFIG_SCHEMA 1U
#define MEMO_CONFIG_URL_MAX 240U
#define MEMO_CONFIG_DEVICE_MAX 39U
#define MEMO_CONFIG_TOKEN_LENGTH 64U
#define MEMO_CONFIG_BLOB_MAX 512U

typedef struct { uint32_t owner; } memo_config_activity_gate_t;
typedef enum { MEMO_ACTIVITY_IDLE, MEMO_ACTIVITY_AUDIO, MEMO_ACTIVITY_COMMIT, MEMO_ACTIVITY_MEDIA } memo_config_activity_t;
bool memo_config_activity_acquire(memo_config_activity_gate_t *, memo_config_activity_t);
bool memo_config_activity_release(memo_config_activity_gate_t *, memo_config_activity_t);
memo_config_activity_t memo_config_activity(const memo_config_activity_gate_t *);

typedef enum {
    MEMO_CONFIG_OK, MEMO_CONFIG_INVALID, MEMO_CONFIG_STORAGE_ERROR,
    MEMO_CONFIG_UNSUPPORTED, MEMO_CONFIG_BUSY, MEMO_CONFIG_NOT_CONFIGURED,
    MEMO_CONFIG_CONFLICT
} memo_config_result_t;
typedef enum { MEMO_CONFIG_IO_OK, MEMO_CONFIG_IO_NOT_FOUND, MEMO_CONFIG_IO_ERROR } memo_config_io_t;
typedef struct {
    void *context;
    memo_config_io_t (*read)(void *, const char *, uint8_t *, size_t *);
    memo_config_io_t (*write)(void *, const char *, const uint8_t *, size_t);
    memo_config_io_t (*commit)(void *);
} memo_config_storage_t;
typedef struct { uint8_t bytes[16]; } memo_server_uuid_t;
typedef enum { MEMO_SERVICE_UNPAIRED, MEMO_SERVICE_PAIRING, MEMO_SERVICE_PAIRED } memo_service_phase_t;
/* One current service, not an old/new service history. Pending values are
 * persisted before HTTP. This internal snapshot contains secrets: no UI/logs. */
typedef struct {
    uint64_t generation;
    bool enabled;
    memo_service_phase_t phase;
    char base_url[MEMO_CONFIG_URL_MAX + 1];
    char device_id[MEMO_CONFIG_DEVICE_MAX + 1];
    char token[MEMO_CONFIG_TOKEN_LENGTH + 1];
    char enrollment_id[37];
    char pairing_code[17];
    memo_server_uuid_t instance;
} memo_config_snapshot_t;
typedef struct {
    memo_config_storage_t storage;
    memo_config_snapshot_t value;
    bool loaded, needs_reload;
    memo_config_result_t result;
} memo_config_t;
typedef struct {
    uint64_t generation;
    bool enabled, configured, pairing;
    memo_config_result_t result;
    char base_url[MEMO_CONFIG_URL_MAX + 1];
    memo_server_uuid_t instance;
} memo_config_public_t;

memo_config_result_t memo_config_validate_url(const char *, char *, size_t);
memo_config_result_t memo_config_join_url(const char *, const char *, char *, size_t);
bool memo_config_uuid_parse(const char *, memo_server_uuid_t *);
bool memo_config_uuid_equal(const memo_server_uuid_t *, const memo_server_uuid_t *);
memo_config_result_t memo_config_load(memo_config_t *, const memo_config_storage_t *);
void memo_config_public_status(const memo_config_t *, memo_config_public_t *);
memo_config_result_t memo_config_read_snapshot(const memo_config_t *, memo_config_snapshot_t *);
memo_config_result_t memo_config_credentials(const memo_config_t *, memo_config_snapshot_t *);
/* Caller serializes configuration/HTTP starts and holds the SHORT audio commit
 * gate around mutations, never across HTTP. Failed persistence has unknown
 * outcome: reload durable bytes before further mutation/credential access. */
memo_config_result_t memo_config_set_service(memo_config_t *, const char *device,
    const char *url, const char *code, const char *enrollment_id, const char *token, bool local_only);
memo_config_result_t memo_config_bind_instance(memo_config_t *, uint64_t generation, const memo_server_uuid_t *);
memo_config_result_t memo_config_finish_pairing(memo_config_t *, uint64_t generation, const memo_server_uuid_t *);
memo_config_result_t memo_config_reject_pairing(memo_config_t *, uint64_t generation);

#ifdef __cplusplus
}
#endif
#endif
