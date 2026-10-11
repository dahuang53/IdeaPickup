/* URL/UUID/activity primitives extracted from DONOR; current-service storage below. */
#include "memo_network_config.h"

bool memo_config_activity_acquire(memo_config_activity_gate_t *gate, memo_config_activity_t owner)
{
    if (!gate || (owner != MEMO_ACTIVITY_AUDIO && owner != MEMO_ACTIVITY_COMMIT)) return false;
    uint32_t expected = MEMO_ACTIVITY_IDLE;
    return __atomic_compare_exchange_n(&gate->owner, &expected, (uint32_t)owner, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
bool memo_config_activity_release(memo_config_activity_gate_t *gate, memo_config_activity_t owner)
{
    if (!gate || (owner != MEMO_ACTIVITY_AUDIO && owner != MEMO_ACTIVITY_COMMIT)) return false;
    uint32_t expected = (uint32_t)owner;
    return __atomic_compare_exchange_n(&gate->owner, &expected, MEMO_ACTIVITY_IDLE, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
memo_config_activity_t memo_config_activity(const memo_config_activity_gate_t *gate)
{
    return gate ? (memo_config_activity_t)__atomic_load_n(&gate->owner, __ATOMIC_SEQ_CST) : MEMO_ACTIVITY_IDLE;
}

#include <limits.h>
#include <string.h>


typedef struct { uint8_t *p; size_t cap, n; bool ok; } writer_t;
typedef struct { const uint8_t *p; size_t n, at; bool ok; } reader_t;

static size_t bounded_length(const char *s, size_t maximum)
{
    if (!s) return maximum + 1;
    size_t n = 0;
    while (n <= maximum && s[n]) ++n;
    return n;
}
static bool nonzero(const uint8_t *p, size_t n)
{
    uint8_t value = 0;
    for (size_t i = 0; i < n; ++i) value |= p[i];
    return value != 0;
}
bool memo_config_uuid_equal(const memo_server_uuid_t *a, const memo_server_uuid_t *b)
{
    return a && b && memcmp(a->bytes, b->bytes, 16) == 0;
}
static bool uuid_valid(const memo_server_uuid_t *uuid) { return uuid && nonzero(uuid->bytes, 16); }
static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool memo_config_uuid_parse(const char *text, memo_server_uuid_t *out)
{
    if (!out || bounded_length(text, 36) != 36) return false;
    memo_server_uuid_t parsed = {{0}};
    size_t byte = 0;
    for (size_t i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i++] != '-') return false;
            continue;
        }
        int a = hex((unsigned char)text[i++]), b = hex((unsigned char)text[i++]);
        if (a < 0 || b < 0 || byte >= sizeof(parsed.bytes)) return false;
        parsed.bytes[byte++] = (uint8_t)((a << 4) | b);
    }
    if (!uuid_valid(&parsed)) return false;
    *out = parsed;
    return true;
}
static bool ascii_id(const char *s, size_t maximum)
{
    size_t n = bounded_length(s, maximum);
    if (!n || n > maximum) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}
static bool token_valid(const char *s)
{
    if (bounded_length(s, MEMO_CONFIG_TOKEN_LENGTH) != MEMO_CONFIG_TOKEN_LENGTH) return false;
    for (size_t i = 0; i < MEMO_CONFIG_TOKEN_LENGTH; ++i) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
static bool utf8_clean(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        unsigned char c = (unsigned char)s[i++];
        unsigned tail;
        if (c < 0x80) { cp = c; tail = 0; }
        else if (c >= 0xc2 && c <= 0xdf) { cp = c & 0x1fU; tail = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 0x0fU; tail = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 0x07U; tail = 3; }
        else return false;
        if (i + tail > n) return false;
        for (unsigned j = 0; j < tail; ++j) {
            unsigned char t = (unsigned char)s[i++];
            if ((t & 0xc0U) != 0x80U) return false;
            cp = (cp << 6) | (t & 0x3fU);
        }
        if ((tail == 1 && cp < 0x80) || (tail == 2 && cp < 0x800) ||
            (tail == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) return false;
        if (cp <= 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp == 0xa0 ||
            (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
            cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == '\\') return false;
    }
    return true;
}
static bool ipv4_valid(const char *s, size_t n)
{
    size_t at = 0;
    for (unsigned group = 0; group < 4; ++group) {
        size_t begin = at;
        unsigned value = 0;
        while (at < n && s[at] >= '0' && s[at] <= '9') {
            value = value * 10U + (unsigned)(s[at++] - '0');
            if (value > 255 || at - begin > 3) return false;
        }
        if (at == begin || (at - begin > 1 && s[begin] == '0')) return false;
        if (group == 3) return at == n;
        if (at == n || s[at++] != '.') return false;
    }
    return false;
}
static bool ipv6_valid(const char *s, size_t n)
{
    size_t at = 0;
    unsigned groups = 0;
    bool compressed = false;
    if (!n) return false;
    if (s[0] == ':') {
        if (n < 2 || s[1] != ':') return false;
        compressed = true; at = 2;
    }
    while (at < n) {
        size_t begin = at;
        while (at < n && hex((unsigned char)s[at]) >= 0) ++at;
        if (at < n && s[at] == '.') {
            if (!ipv4_valid(s + begin, n - begin)) return false;
            groups += 2; at = n; break;
        }
        if (at == begin || at - begin > 4 || ++groups > 8) return false;
        if (at == n) break;
        if (s[at++] != ':') return false;
        if (at < n && s[at] == ':') {
            if (compressed) return false;
            compressed = true; ++at;
        } else if (at == n) return false;
    }
    return compressed ? groups < 8 : groups == 8;
}
/* The input has already passed ipv6_valid. Canonical output matches the
 * server's compressed IPv6 spelling, including embedded IPv4 addresses. */
static size_t ipv6_canonical(const char *s, size_t n, char out[40])
{
    uint16_t words[8] = {0}; unsigned count = 0, gap = 8; size_t at = 0;
    if (s[0] == ':') { gap = 0; at = 2; }
    while (at < n) {
        size_t begin = at; unsigned value = 0;
        while (at < n && hex((unsigned char)s[at]) >= 0) value = value * 16U + (unsigned)hex((unsigned char)s[at++]);
        if (at < n && s[at] == '.') {
            unsigned octets[4] = {0}; at = begin;
            for (unsigned i = 0; i < 4; ++i) {
                while (at < n && s[at] != '.') octets[i] = octets[i] * 10U + (unsigned)(s[at++] - '0');
                if (at < n) ++at;
            }
            words[count++] = (uint16_t)((octets[0] << 8) | octets[1]);
            words[count++] = (uint16_t)((octets[2] << 8) | octets[3]);
            break;
        }
        words[count++] = (uint16_t)value;
        if (at < n) ++at;
        if (at < n && s[at] == ':') { gap = count; ++at; }
    }
    if (gap < 8) {
        unsigned missing = 8 - count;
        for (unsigned i = count; i > gap; --i) words[i - 1 + missing] = words[i - 1];
        for (unsigned i = gap; i < gap + missing; ++i) words[i] = 0;
    }
    unsigned best_start = 8, best_length = 0;
    for (unsigned i = 0; i < 8;) {
        unsigned start = i;
        while (i < 8 && !words[i]) ++i;
        if (i - start > best_length && i - start >= 2) { best_start = start; best_length = i - start; }
        if (i == start) ++i;
    }
    size_t length = 0;
    for (unsigned i = 0; i < 8;) {
        if (i == best_start) { out[length++] = ':'; out[length++] = ':'; i += best_length; continue; }
        if (length && out[length - 1] != ':') out[length++] = ':';
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            unsigned digit = (words[i] >> shift) & 15U;
            if (digit || started || shift == 0) { out[length++] = "0123456789abcdef"[digit]; started = true; }
        }
        ++i;
    }
    out[length] = 0; return length;
}
static bool dns_valid(const char *s, size_t n)
{
    if (!n || n > 253) return false;
    bool numeric = true;
    for (size_t i = 0; i < n; ++i) if ((s[i] < '0' || s[i] > '9') && s[i] != '.') numeric = false;
    if (numeric) return ipv4_valid(s, n);
    size_t label = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (i == n || s[i] == '.') {
            if (!label || label > 63 || s[i - 1] == '-') return false;
            label = 0; continue;
        }
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) return false;
        if (!label && c == '-') return false;
        ++label;
    }
    return true;
}
static bool path_clean(const char *s, size_t n)
{
    if (!utf8_clean(s, n)) return false;
    uint8_t decoded[MEMO_CONFIG_BLOB_MAX + 1];
    if (n > MEMO_CONFIG_BLOB_MAX) return false;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '?' || s[i] == '#') return false;
        if (s[i] == '%') {
            if (i + 2 >= n) return false;
            int a = hex((unsigned char)s[i + 1]), b = hex((unsigned char)s[i + 2]);
            if (a < 0 || b < 0) return false;
            unsigned c = (unsigned)((a << 4) | b);
            if (c <= 0x20 || c == 0x7f || c == '\\' || c == '/') return false;
            decoded[count++] = (uint8_t)c;
            i += 2;
        } else decoded[count++] = (uint8_t)s[i];
    }
    if (!utf8_clean((const char *)decoded, count)) return false;
    for (size_t i = 0; i < count;) {
        if (decoded[i] == '/') {
            if (i + 1 < count && decoded[i + 1] == '/') return false;
            ++i;
        }
        size_t begin = i;
        while (i < count && decoded[i] != '/') ++i;
        if ((i - begin == 1 && decoded[begin] == '.') ||
            (i - begin == 2 && decoded[begin] == '.' && decoded[begin + 1] == '.')) return false;
    }
    return true;
}
memo_config_result_t memo_config_validate_url(const char *input, char *normalized, size_t capacity)
{
    size_t n = bounded_length(input, MEMO_CONFIG_URL_MAX);
    if (n < 9 || n > MEMO_CONFIG_URL_MAX || !utf8_clean(input, n)) return MEMO_CONFIG_INVALID;
    const char *scheme = "https://";
    for (size_t i = 0; i < 8; ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        if (c != scheme[i]) return MEMO_CONFIG_INVALID;
    }
    size_t end = 8;
    while (end < n && input[end] != '/') ++end;
    size_t host_end; char ipv6[40] = {0}; size_t ipv6_length = 0;
    if (input[8] == '[') {
        host_end = 9;
        while (host_end < end && input[host_end] != ']') ++host_end;
        if (host_end == end || !ipv6_valid(input + 9, host_end - 9)) return MEMO_CONFIG_INVALID;
        ipv6_length = ipv6_canonical(input + 9, host_end - 9, ipv6);
        ++host_end;
    } else {
        host_end = 8;
        while (host_end < end && input[host_end] != ':') ++host_end;
        if (!dns_valid(input + 8, host_end - 8)) return MEMO_CONFIG_INVALID;
    }
    unsigned port = 0;
    if (host_end < end) {
        size_t port_begin = host_end + 1;
        if (input[host_end] != ':' || port_begin == end) return MEMO_CONFIG_INVALID;
        for (size_t i = port_begin; i < end; ++i) {
            if (input[i] < '0' || input[i] > '9' || i - port_begin >= 5) return MEMO_CONFIG_INVALID;
            port = port * 10U + (unsigned)(input[i] - '0');
        }
        if (!port || port > 65535 || (end - port_begin > 1 && input[port_begin] == '0')) return MEMO_CONFIG_INVALID;
    }
    while (n > end && input[n - 1] == '/') --n;
    if (!path_clean(input + end, n - end)) return MEMO_CONFIG_INVALID;
    if (normalized) {
        char result[MEMO_CONFIG_URL_MAX + 1]; size_t length = 8;
        memcpy(result, "https://", length);
        if (ipv6_length) {
            result[length++] = '['; memcpy(result + length, ipv6, ipv6_length); length += ipv6_length; result[length++] = ']';
        } else {
            for (size_t i = 8; i < host_end; ++i) {
                char c = input[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; result[length++] = c;
            }
        }
        if (port && port != 443) { memcpy(result + length, input + host_end, end - host_end); length += end - host_end; }
        memcpy(result + length, input + end, n - end); length += n - end; result[length] = 0;
        if (capacity <= length) return MEMO_CONFIG_INVALID;
        memcpy(normalized, result, length + 1);
    }
    return MEMO_CONFIG_OK;
}
memo_config_result_t memo_config_join_url(const char *base, const char *path, char *out, size_t capacity)
{
    char normalized[MEMO_CONFIG_URL_MAX + 1];
    if (!out || !capacity) return MEMO_CONFIG_INVALID;
    out[0] = 0;
    if (memo_config_validate_url(base, normalized, sizeof(normalized)) != MEMO_CONFIG_OK || !path ||
        path[0] != '/' || path[1] == '/') return MEMO_CONFIG_INVALID;
    size_t a = strlen(normalized), b = bounded_length(path, MEMO_CONFIG_BLOB_MAX);
    if (b > MEMO_CONFIG_BLOB_MAX || !path_clean(path, b)) return MEMO_CONFIG_INVALID;
    /* ESP sends the request target as URI bytes. Encode raw UTF-8 (and unsafe
     * ASCII) without changing the persisted base or double-encoding escapes. */
    const char *parts[2] = {normalized, path}; const size_t sizes[2] = {a, b};
    const char *safe = ":/[]@!$&'()*+,;=-._~%"; size_t needed = 0;
    for (unsigned p = 0; p < 2; ++p) for (size_t i = 0; i < sizes[p]; ++i) {
        unsigned char c = (unsigned char)parts[p][i];
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c < 128 && strchr(safe, c));
        needed += plain ? 1 : 3;
    }
    if (needed >= capacity) return MEMO_CONFIG_INVALID;
    size_t at = 0;
    for (unsigned p = 0; p < 2; ++p) for (size_t i = 0; i < sizes[p]; ++i) {
        unsigned char c = (unsigned char)parts[p][i];
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c < 128 && strchr(safe, c));
        if (plain) out[at++] = (char)c;
        else { out[at++] = '%'; out[at++] = "0123456789ABCDEF"[c >> 4]; out[at++] = "0123456789ABCDEF"[c & 15]; }
    }
    out[at] = 0;
    return MEMO_CONFIG_OK;
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
static void w8(writer_t *w, uint8_t v) { if (w->n < w->cap) w->p[w->n++] = v; else w->ok = false; }
static void w32(writer_t *w, uint32_t v) { for (unsigned i = 0; i < 4; ++i) w8(w, (uint8_t)(v >> (i * 8))); }
static void w64(writer_t *w, uint64_t v) { for (unsigned i = 0; i < 8; ++i) w8(w, (uint8_t)(v >> (i * 8))); }
static void ws(writer_t *w, const char *s) { size_t n = strlen(s); w8(w, (uint8_t)n); for (size_t i = 0; i < n; ++i) w8(w, (uint8_t)s[i]); }
static uint8_t r8(reader_t *r) { if (r->at < r->n) return r->p[r->at++]; r->ok = false; return 0; }
static uint32_t r32(reader_t *r) { uint32_t v = 0; for (unsigned i = 0; i < 4; ++i) v |= (uint32_t)r8(r) << (i * 8); return v; }
static uint64_t r64(reader_t *r) { uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)r8(r) << (i * 8); return v; }
static void rs(reader_t *r, char *out, size_t capacity)
{
    size_t n = r8(r);
    if (n >= capacity || n > r->n - r->at) { r->ok = false; return; }
    for (size_t i = 0; i < n; ++i) { out[i] = (char)r8(r); if (!out[i]) r->ok = false; }
    out[n] = 0;
}
static bool code_valid(const char *s)
{
    size_t n = bounded_length(s, 16);
    if (n < 10 || n > 16) return false;
    for (size_t i = 0; i < n; ++i) if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '2' && s[i] <= '9'))) return false;
    return true;
}
static bool snapshot_valid(const memo_config_snapshot_t *s)
{
    if (s->phase > MEMO_SERVICE_PAIRED) return false;
    char url[MEMO_CONFIG_URL_MAX + 1];
    if (s->base_url[0] && (memo_config_validate_url(s->base_url, url, sizeof(url)) != MEMO_CONFIG_OK || strcmp(s->base_url, url))) return false;
    if (s->device_id[0] && !ascii_id(s->device_id, MEMO_CONFIG_DEVICE_MAX)) return false;
    if (s->phase == MEMO_SERVICE_UNPAIRED)
        return !s->token[0] && !s->enrollment_id[0] && !s->pairing_code[0] && !nonzero(s->instance.bytes, 16);
    if (!s->base_url[0] || !s->device_id[0] || !token_valid(s->token)) return false;
    if (s->phase == MEMO_SERVICE_PAIRED)
        return uuid_valid(&s->instance) && !s->pairing_code[0] && !s->enrollment_id[0];
    memo_server_uuid_t identifier;
    return code_valid(s->pairing_code) && memo_config_uuid_parse(s->enrollment_id, &identifier);
}
static size_t encode(uint8_t *out, const memo_config_snapshot_t *s)
{
    writer_t w = {out, MEMO_CONFIG_BLOB_MAX, 0, true};
    w32(&w, 0x31434d56U); w32(&w, MEMO_CONFIG_SCHEMA); w64(&w, s->generation);
    w8(&w, s->enabled ? 1 : 0); w8(&w, (uint8_t)s->phase);
    ws(&w, s->base_url); ws(&w, s->device_id); ws(&w, s->token); ws(&w, s->enrollment_id); ws(&w, s->pairing_code);
    for (unsigned i = 0; i < 16; ++i) w8(&w, s->instance.bytes[i]);
    uint32_t crc = crc32(out, w.n); w32(&w, crc);
    return w.ok ? w.n : 0;
}
static memo_config_result_t decode(const uint8_t *bytes, size_t length, memo_config_snapshot_t *out)
{
    if (length < 8 || length > MEMO_CONFIG_BLOB_MAX) return MEMO_CONFIG_STORAGE_ERROR;
    reader_t r = {bytes, length, 0, true};
    if (r32(&r) != 0x31434d56U) return MEMO_CONFIG_STORAGE_ERROR;
    if (r32(&r) != MEMO_CONFIG_SCHEMA) return MEMO_CONFIG_UNSUPPORTED;
    if (length < 43) return MEMO_CONFIG_STORAGE_ERROR;
    memo_config_snapshot_t s = {0}; s.generation = r64(&r);
    uint8_t enabled = r8(&r); s.enabled = enabled == 1; s.phase = (memo_service_phase_t)r8(&r);
    rs(&r, s.base_url, sizeof(s.base_url)); rs(&r, s.device_id, sizeof(s.device_id)); rs(&r, s.token, sizeof(s.token));
    rs(&r, s.enrollment_id, sizeof(s.enrollment_id)); rs(&r, s.pairing_code, sizeof(s.pairing_code));
    for (unsigned i = 0; i < 16; ++i) s.instance.bytes[i] = r8(&r);
    uint32_t crc = r32(&r);
    if (!r.ok || r.at != r.n || enabled > 1 || !s.generation || crc32(bytes, length - 4) != crc || !snapshot_valid(&s)) return MEMO_CONFIG_STORAGE_ERROR;
    *out = s; return MEMO_CONFIG_OK;
}
memo_config_result_t memo_config_load(memo_config_t *c, const memo_config_storage_t *storage)
{
    if (!c || !storage || !storage->read || !storage->write || !storage->commit) return MEMO_CONFIG_INVALID;
    memo_config_storage_t io = *storage; memset(c, 0, sizeof(*c)); c->storage = io;
    uint8_t bytes[MEMO_CONFIG_BLOB_MAX]; size_t length = sizeof(bytes);
    memo_config_io_t read = io.read(io.context, "service", bytes, &length);
    c->result = read == MEMO_CONFIG_IO_NOT_FOUND ? MEMO_CONFIG_OK :
        read == MEMO_CONFIG_IO_OK ? decode(bytes, length, &c->value) : MEMO_CONFIG_STORAGE_ERROR;
    /* A transient read failure remains retryable. A successfully read corrupt
     * or unsupported value is never erased/replaced as a fresh installation. */
    c->needs_reload = read == MEMO_CONFIG_IO_ERROR;
    memset(bytes, 0, sizeof(bytes)); c->loaded = c->result == MEMO_CONFIG_OK;
    return c->result;
}
static memo_config_result_t usable(const memo_config_t *c)
{
    if (!c) return MEMO_CONFIG_INVALID;
    return c->needs_reload ? MEMO_CONFIG_STORAGE_ERROR : !c->loaded ?
        (c->result == MEMO_CONFIG_OK ? MEMO_CONFIG_STORAGE_ERROR : c->result) : MEMO_CONFIG_OK;
}
static memo_config_result_t publish(memo_config_t *c, memo_config_snapshot_t *next)
{
    memo_config_result_t result = usable(c); if (result != MEMO_CONFIG_OK) return result;
    if (!snapshot_valid(next) || c->value.generation == UINT64_MAX) return MEMO_CONFIG_INVALID;
    next->generation = c->value.generation + 1;
    uint8_t bytes[MEMO_CONFIG_BLOB_MAX], actual[MEMO_CONFIG_BLOB_MAX];
    size_t n = encode(bytes, next), length = sizeof(actual);
    bool ok = n && c->storage.write(c->storage.context, "service", bytes, n) == MEMO_CONFIG_IO_OK &&
        c->storage.commit(c->storage.context) == MEMO_CONFIG_IO_OK &&
        c->storage.read(c->storage.context, "service", actual, &length) == MEMO_CONFIG_IO_OK && length == n && !memcmp(bytes, actual, n);
    memset(bytes, 0, sizeof(bytes)); memset(actual, 0, sizeof(actual));
    if (!ok) { c->needs_reload = true; c->result = MEMO_CONFIG_STORAGE_ERROR; return c->result; }
    c->value = *next; return MEMO_CONFIG_OK;
}
memo_config_result_t memo_config_read_snapshot(const memo_config_t *c, memo_config_snapshot_t *out)
{
    if (!out) return MEMO_CONFIG_INVALID;
    memset(out, 0, sizeof(*out)); memo_config_result_t result = usable(c);
    if (result == MEMO_CONFIG_OK) *out = c->value;
    return result;
}
memo_config_result_t memo_config_credentials(const memo_config_t *c, memo_config_snapshot_t *out)
{
    memo_config_result_t result = memo_config_read_snapshot(c, out);
    if (result != MEMO_CONFIG_OK) return result;
    if (!out->enabled || out->phase != MEMO_SERVICE_PAIRED) { memset(out, 0, sizeof(*out)); return MEMO_CONFIG_NOT_CONFIGURED; }
    return MEMO_CONFIG_OK;
}
void memo_config_public_status(const memo_config_t *c, memo_config_public_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out)); out->result = usable(c);
    if (out->result != MEMO_CONFIG_OK) return;
    out->generation = c->value.generation; out->enabled = c->value.enabled;
    out->configured = c->value.enabled && c->value.phase == MEMO_SERVICE_PAIRED;
    out->pairing = c->value.enabled && c->value.phase == MEMO_SERVICE_PAIRING;
    memcpy(out->base_url, c->value.base_url, sizeof(out->base_url)); out->instance = c->value.instance;
}
memo_config_result_t memo_config_set_service(memo_config_t *c, const char *device, const char *url,
    const char *code, const char *enrollment_id, const char *token, bool local_only)
{
    memo_config_result_t result = usable(c); if (result != MEMO_CONFIG_OK) return result;
    if (!ascii_id(device, MEMO_CONFIG_DEVICE_MAX)) return MEMO_CONFIG_INVALID;
    memo_config_snapshot_t next = c->value;
    if (local_only) { if (!next.enabled) return MEMO_CONFIG_OK; next.enabled = false; return publish(c, &next); }
    char normalized[MEMO_CONFIG_URL_MAX + 1];
    if (memo_config_validate_url(url, normalized, sizeof(normalized)) != MEMO_CONFIG_OK) return MEMO_CONFIG_INVALID;
    bool same = !strcmp(normalized, next.base_url) && !strcmp(device, next.device_id);
    if (same && (!code || !*code) && next.enabled) return MEMO_CONFIG_OK;
    if (same && next.phase == MEMO_SERVICE_PAIRING && code && *code) {
        if (strcmp(next.pairing_code, code)) return MEMO_CONFIG_BUSY;
        next.enabled = true; return next.enabled == c->value.enabled ? MEMO_CONFIG_OK : publish(c, &next);
    }
    if (!same || (code && *code)) {
        memset(&next, 0, sizeof(next)); memcpy(next.base_url, normalized, strlen(normalized) + 1);
        memcpy(next.device_id, device, strlen(device) + 1);
        if (code && *code) {
            memo_server_uuid_t identifier;
            if (!code_valid(code) || !memo_config_uuid_parse(enrollment_id, &identifier) || !token_valid(token)) return MEMO_CONFIG_INVALID;
            for (size_t i = 0; i < 36; ++i) if (enrollment_id[i] >= 'A' && enrollment_id[i] <= 'F') return MEMO_CONFIG_INVALID;
            memcpy(next.token, token, 65); memcpy(next.enrollment_id, enrollment_id, 37); memcpy(next.pairing_code, code, strlen(code) + 1);
            next.phase = MEMO_SERVICE_PAIRING;
        }
    }
    next.enabled = true; return publish(c, &next);
}
memo_config_result_t memo_config_bind_instance(memo_config_t *c, uint64_t generation, const memo_server_uuid_t *instance)
{
    memo_config_result_t result = usable(c); if (result != MEMO_CONFIG_OK) return result;
    if (c->value.generation != generation) return MEMO_CONFIG_CONFLICT;
    if (c->value.phase != MEMO_SERVICE_PAIRING || !uuid_valid(instance)) return MEMO_CONFIG_INVALID;
    if (uuid_valid(&c->value.instance)) return memo_config_uuid_equal(instance, &c->value.instance) ? MEMO_CONFIG_OK : MEMO_CONFIG_CONFLICT;
    memo_config_snapshot_t next = c->value; next.instance = *instance; return publish(c, &next);
}
memo_config_result_t memo_config_finish_pairing(memo_config_t *c, uint64_t generation, const memo_server_uuid_t *instance)
{
    memo_config_result_t result = usable(c); if (result != MEMO_CONFIG_OK) return result;
    if (c->value.generation != generation) return MEMO_CONFIG_CONFLICT;
    if (c->value.phase != MEMO_SERVICE_PAIRING || !memo_config_uuid_equal(instance, &c->value.instance) || !uuid_valid(instance)) return MEMO_CONFIG_INVALID;
    memo_config_snapshot_t next = c->value; next.phase = MEMO_SERVICE_PAIRED; next.pairing_code[0] = 0; next.enrollment_id[0] = 0;
    return publish(c, &next);
}
memo_config_result_t memo_config_reject_pairing(memo_config_t *c, uint64_t generation)
{
    memo_config_result_t result = usable(c); if (result != MEMO_CONFIG_OK) return result;
    if (c->value.generation != generation) return MEMO_CONFIG_CONFLICT;
    if (c->value.phase != MEMO_SERVICE_PAIRING) return MEMO_CONFIG_INVALID;
    memo_config_snapshot_t next = c->value; next.phase = MEMO_SERVICE_UNPAIRED;
    memset(next.token, 0, sizeof(next.token)); memset(next.enrollment_id, 0, sizeof(next.enrollment_id));
    memset(next.pairing_code, 0, sizeof(next.pairing_code)); memset(&next.instance, 0, sizeof(next.instance));
    return publish(c, &next);
}
