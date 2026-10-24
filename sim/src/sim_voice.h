// Simulator-only voice link injection (implemented in voice_stub.cpp).
//
// The firmware's voice task and BLE service are not compiled into the host
// build, so the assistant phases (listening / thinking / speaking / ...) would
// otherwise always read as "idle". The control server uses these hooks to put
// the Qiuqiu avatar into every phase without a radio.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same numbering as voice_state_t in main/voice/voice_state.h. */
void sim_voice_set_state(int state);
int  sim_voice_get_state(void);

/* Link presence: disconnect drops the link and reports "sleepy", connect
   restores it. Either call also marks the "ever connected" flag the avatar
   uses to tell "never paired" apart from "lost the link". */
void sim_voice_set_connected(bool connected);

#ifdef __cplusplus
}
#endif
