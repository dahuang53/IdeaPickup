#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* cJSON exposes NUL-terminated strings without decoded lengths. Reject a
 * decoded NUL before parsing, so an address/action/device ID cannot be silently
 * shortened at \u0000. Escaped literal backslashes remain ordinary text.
 * Limit structural depth before cJSON's recursive parser uses task stack.
 * This is a transport precheck, not a replacement for JSON parsing. */
static inline bool memo_service_json_text_safe(const char *text, size_t length)
{
    if (!text) return false;
    unsigned depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < length; ++i) {
        if (!text[i]) return false;
        if (in_string) {
            if ((unsigned char)text[i] < 0x20) return false;
            if (text[i] == '\\') {
                if (i + 1 == length) return false;
                if (i + 5 < length && text[i + 1] == 'u' && text[i + 2] == '0' &&
                    text[i + 3] == '0' && text[i + 4] == '0' && text[i + 5] == '0') return false;
                if (!text[++i]) return false;
            } else if (text[i] == '"') in_string = false;
        } else if (text[i] == '"') in_string = true;
        else if (text[i] == '{' || text[i] == '[') {
            if (++depth > 16) return false;
        } else if (text[i] == '}' || text[i] == ']') {
            if (!depth) return false;
            --depth;
        } else if (text[i] == '\\') {
            return false;
        }
    }
    return !in_string && depth == 0;
}
/* Current-service portal adapters. Settings commit through the existing short
 * admission gate; pairing HTTP runs on the ordinary application worker. */
typedef bool (*memo_service_status_fn)(char *, size_t);
typedef bool (*memo_service_apply_fn)(const char *, size_t, char *, size_t);
void memo_service_portal_register(memo_service_status_fn, memo_service_apply_fn);
#ifdef __cplusplus
}
#endif
