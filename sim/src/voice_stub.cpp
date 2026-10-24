// Voice/BLE stubs: the simulator has no radio.
//
// The real voice_task.cpp polls the buttons, drives I2S and talks NimBLE, so it
// is not part of the host build. What the UI consumes is only the link state,
// which the control server injects through sim_voice.h (`voice <state>`), so
// the assistant phases can be screenshotted and regression-tested.
#include <cstdint>
#include <cstddef>

#include "sim_voice.h"

namespace {
/* Same order as voice_state_t in main/voice/voice_state.h. */
constexpr int kIdle      = 0;
constexpr int kListening = 1;
constexpr int kThinking  = 2;
constexpr int kHappy     = 3;
constexpr int kSleepy    = 4;
constexpr int kSpeaking  = 5;

int  g_state          = kIdle;
bool g_connected      = false;
bool g_ever_connected = false;
}  // namespace

extern "C" {
int  voice_state_get() { return g_state; }
bool voice_state_connected() { return g_connected; }
bool voice_state_ever_connected() { return g_ever_connected; }
void voice_set_ptt_enabled(bool) {}
bool voice_get_ptt_enabled() { return false; }
void voice_start() {}
void ble_svc_debug_send(const uint8_t*, size_t) {}
bool ble_svc_take_command(uint8_t*, uint8_t*) { return false; }
void ble_svc_requeue_command() {}

void sim_voice_set_state(int state)
{
    if (state < kIdle || state > kSpeaking) return;
    g_state = state;
    if (state != kIdle) {
        /* any active phase implies the link exists */
        g_connected      = true;
        g_ever_connected = true;
    }
}

int sim_voice_get_state() { return g_state; }

void sim_voice_set_connected(bool connected)
{
    g_connected = connected;
    if (connected) {
        g_ever_connected = true;
        if (g_state == kSleepy) g_state = kIdle;
    } else {
        g_state = kSleepy;   /* the avatar shows "sleepy" only after a drop */
    }
}
}
