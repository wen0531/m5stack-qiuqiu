#include "sim_input.h"

namespace sim_input {
namespace {
bool a_down = false, b_down = false;
bool t_down = false;
int  tx = 0, ty = 0;
}  // namespace

void set_key(char key, bool pressed)
{
    if (key == 'A' || key == 'a') a_down = pressed;
    else if (key == 'B' || key == 'b') b_down = pressed;
}
bool get_key(char key) { return (key == 'A' || key == 'a') ? a_down : b_down; }

void set_touch(int x, int y, bool pressed)
{
    t_down = pressed;
    tx = x;
    ty = y;
}
bool touch_pressed() { return t_down; }
int  touch_x() { return tx; }
int  touch_y() { return ty; }
}  // namespace sim_input
