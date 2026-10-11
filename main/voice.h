#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef enum { VOICE_OFF, VOICE_IDLE, VOICE_RECORDING, VOICE_USB_MIC } voice_mode_t;
bool voice_init(void);
void voice_set_mode(voice_mode_t mode);
void voice_submit(const int16_t *pcm, size_t samples);
bool voice_request_valid(uint32_t generation);
// App worker calls this after processing or rejecting a queued voice command.
// An old-generation acknowledgement cannot unlock a newer voice window.
void voice_command_ack(uint32_t generation);
void voice_status(bool *ready, bool *window, uint32_t *drops);
