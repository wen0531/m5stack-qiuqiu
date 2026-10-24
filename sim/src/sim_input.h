// Shared input state between the control server and the HAL stub.
#pragma once
namespace sim_input {
void set_key(char key, bool pressed);   // 'A' or 'B'
bool get_key(char key);
void set_touch(int x, int y, bool pressed);
bool touch_pressed();
int  touch_x();
int  touch_y();
}  // namespace sim_input
