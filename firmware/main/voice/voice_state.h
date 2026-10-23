// Shared voice link state, updated by voice_task, read by UI apps.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_IDLE = 0,
    VOICE_LISTENING, /* push-to-talk capturing and streaming */
    VOICE_THINKING,  /* talk finished, waiting for the agent */
    VOICE_HAPPY,     /* BLE just connected */
    VOICE_SLEEPY,    /* BLE not connected */
    VOICE_SPEAKING,  /* downlink audio is playing */
} voice_state_t;

voice_state_t voice_state_get(void);
bool voice_state_ever_connected(void);
bool voice_state_connected(void);

/* Push-to-talk is only armed while the Qiuqiu app is in the foreground:
 * A long-press must not start a recording from the launcher or other apps. */
void voice_set_ptt_enabled(bool enabled);
bool voice_get_ptt_enabled(void);

/* start BLE service + capture task */
void voice_start(void);

#ifdef __cplusplus
}
#endif
