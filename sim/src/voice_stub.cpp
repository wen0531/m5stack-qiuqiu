// Voice/BLE stubs: the simulator has no radio.
#include <cstdint>
#include <cstddef>

extern "C" {
int  voice_state_get() { return 0; }
bool voice_state_ever_connected() { return false; }
void voice_set_ptt_enabled(bool) {}
void ble_svc_debug_send(const uint8_t*, size_t) {}
bool ble_svc_take_command(uint8_t*, uint8_t*) { return false; }
void ble_svc_requeue_command() {}
}
