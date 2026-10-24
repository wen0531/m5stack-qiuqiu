// Headless simulator: runs the real firmware UI (mooncake + apps + LVGL),
// accepts injected input over a local control socket and writes PNG frames.
#include <lvgl.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <i18n/i18n.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "png.h"

using mooncake::GetMooncake;
using mooncake::Mooncake;
#include "sim_input.h"
#include "sim_display.h"
#include "sim_log.h"
#include "mem_guard.h"
#include "sim_clock.h"

static constexpr int W = 466;
static constexpr int H = 466;
static int ctrl_port()
{
    static const int port = [] {
        const char* v = std::getenv("SIM_CTRL_PORT");
        return v ? std::atoi(v) : 9123;
    }();
    return port;
}

static std::mutex g_fb_mutex;   /* serialises screenshot access */
static std::atomic<bool> g_running{true};
static std::atomic<uint32_t> g_frames{0};
static std::atomic<uint32_t> g_loops{0};
static std::atomic<bool>     g_frozen{false};
static std::atomic<bool>     g_deterministic{false};
static std::atomic<uint32_t> g_last_loop_ms{0};
static std::string g_last_log;

static std::string current_app_name()
{
    auto& mc = GetMooncake();
    for (auto& p : mc.getAllAppProps()) {
        if (mc.getAppCurrentState(p.appID) == mooncake::UIAbility::StateForeground) {
            return p.info.name;
        }
    }
    return "";
}
static std::mutex g_log_mutex;

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    sim_display::write_rect(area->x1, area->y1,
                            area->x2 - area->x1 + 1, area->y2 - area->y1 + 1,
                            (const uint16_t*)px_map);
    g_frames++;
    lv_display_flush_ready(disp);
}

/* touch input: forwards the injected touch point to LVGL */
static void indev_read_cb(lv_indev_t*, lv_indev_data_t* data)
{
    if (sim_input::touch_pressed()) {
        data->point.x = (int32_t)sim_input::touch_x();
        data->point.y = (int32_t)sim_input::touch_y();
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void log_cb(lv_log_level_t, const char* buf)
{
    sim_log_add(buf);
    std::fputs(buf, stdout);
}

/* Advance simulated time. In step mode the main loop is paused, so input
   commands have to let time pass for LVGL to process the event. */
static void sim_advance(uint32_t ms)
{
    if (!g_deterministic) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return;
    }
    const uint32_t target = sim_clock::now_ms() + ms;
    sim_clock::set_paused(false);
    while (sim_clock::now_ms() < target && g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sim_clock::set_paused(true);
}

/* ------------------------------------------------------------ ui helpers */
struct UiItem {
    std::string text;
    lv_area_t   area;   /* label itself */
    lv_area_t   hit;    /* nearest clickable ancestor, or the label if none */
    bool        clickable = false;
};

static void collect_labels(lv_obj_t* obj, std::vector<UiItem>& out, int depth = 0)
{
    if (depth > 12 || obj == nullptr) return;
    const uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; ++i) {
        lv_obj_t* child = lv_obj_get_child(obj, (int32_t)i);
        if (child == nullptr) continue;
        if (lv_obj_check_type(child, &lv_label_class)) {
            const char* t = lv_label_get_text(child);
            if (t && *t) {
                UiItem item;
                item.text = t;
                lv_obj_get_coords(child, &item.area);
                item.hit = item.area;
                for (lv_obj_t* p = child; p != nullptr; p = lv_obj_get_parent(p)) {
                    if (lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE) && p != lv_screen_active()) {
                        lv_obj_get_coords(p, &item.hit);
                        item.clickable = true;
                        break;
                    }
                }
                out.push_back(item);
            }
        }
        collect_labels(child, out, depth + 1);
    }
}

/* ---------------------------------------------------------------- control */
static void handle_command(const std::string& cmd, std::string& reply)
{
    char key;
    int x = 0, y = 0, ms = 0, x2 = 0, y2 = 0;
    if (std::sscanf(cmd.c_str(), "key %c %d", &key, &ms) == 2) {
        sim_input::set_key(key, true);
        sim_advance((uint32_t)(ms > 0 ? ms : 80));
        sim_input::set_key(key, false);
        sim_advance(60);
        reply = "ok\n";
    } else if (std::sscanf(cmd.c_str(), "down %c", &key) == 1) {
        sim_input::set_key(key, true);
        reply = "ok\n";
    } else if (std::sscanf(cmd.c_str(), "up %c", &key) == 1) {
        sim_input::set_key(key, false);
        reply = "ok\n";
    } else if (std::sscanf(cmd.c_str(), "tap %d %d", &x, &y) == 2) {
        sim_input::set_touch(x, y, true);
        sim_advance(80);
        sim_input::set_touch(x, y, false);
        sim_advance(120);
        reply = "ok\n";
    } else if (std::sscanf(cmd.c_str(), "swipe %d %d %d %d %d", &x, &y, &ms, &x2, &y2) == 5) {
        /* simple drag: press, interpolate, release */
        const int steps = 20;
        sim_input::set_touch(x, y, true);
        for (int i = 1; i <= steps; ++i) {
            sim_input::set_touch(x + (x2 - x) * i / steps, y + (y2 - y) * i / steps, true);
            sim_advance((uint32_t)(ms / steps));
        }
        sim_input::set_touch(x2, y2, false);
        sim_advance(120);
        reply = "ok\n";
    } else if (cmd == "dump") {
        std::vector<UiItem> items;
        collect_labels(lv_screen_active(), items, 0);
        reply.clear();
        for (auto& it : items) {
            char line[256];
            std::snprintf(line, sizeof(line), "%d,%d,%d,%d\t%d\t%s\n",
                          it.area.x1, it.area.y1, it.area.x2, it.area.y2,
                          it.clickable ? 1 : 0, it.text.c_str());
            reply += line;
        }
        if (reply.empty()) reply = "(no labels)\n";
    } else if (cmd.rfind("tap_text ", 0) == 0) {
        std::string needle = cmd.substr(9);
        std::vector<UiItem> items;
        collect_labels(lv_screen_active(), items, 0);
        bool hit = false;
        for (auto& it : items) {
            if (it.text.find(needle) != std::string::npos) {
                const int cx = (it.hit.x1 + it.hit.x2) / 2;
                const int cy = (it.hit.y1 + it.hit.y2) / 2;
                sim_input::set_touch(cx, cy, true);
                sim_advance(80);
                sim_input::set_touch(cx, cy, false);
                sim_advance(120);
                reply = "ok " + std::to_string(cx) + " " + std::to_string(cy) + "\n";
                hit = true;
                break;
            }
        }
        if (!hit) reply = "not found\n";
    } else if (cmd.rfind("alloc", 0) == 0) {
        /* testing hook: hold N KB of PSRAM (or internal) to exercise the budget */
        int kb = 1024;
        char pool[16] = "psram";
        std::sscanf(cmd.c_str(), "alloc %d %15s", &kb, pool);
        static std::vector<void*> held;
        const bool internal = std::strcmp(pool, "internal") == 0;
        held.push_back(mem_guard::alloc(internal ? mem_guard::POOL_INTERNAL : mem_guard::POOL_PSRAM,
                                        (size_t)kb * 1024));
        char buf[128];
        std::snprintf(buf, sizeof(buf), "allocated %d KB from %s\n", kb, internal ? "internal" : "psram");
        reply = buf;
    } else if (cmd == "memfree") {
        reply = std::string(mem_guard::report()) + "\n";
    } else if (cmd == "frame") {
        std::vector<unsigned char> png;
        sim_display::lock();
        const bool ok = png_to_memory(sim_display::pixels(), W, H, png);
        sim_display::unlock();
        if (!ok) {
            reply = "error\n";
        } else {
            static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            reply.clear();
            reply.reserve(png.size() * 4 / 3 + 8);
            size_t i = 0;
            for (; i + 2 < png.size(); i += 3) {
                const uint32_t v = (png[i] << 16) | (png[i + 1] << 8) | png[i + 2];
                reply += tbl[(v >> 18) & 63];
                reply += tbl[(v >> 12) & 63];
                reply += tbl[(v >> 6) & 63];
                reply += tbl[v & 63];
            }
            if (i < png.size()) {
                uint32_t v = png[i] << 16;
                if (i + 1 < png.size()) v |= png[i + 1] << 8;
                reply += tbl[(v >> 18) & 63];
                reply += tbl[(v >> 12) & 63];
                reply += (i + 1 < png.size()) ? tbl[(v >> 6) & 63] : '=';
                reply += '=';
            }
            reply += "\n";
        }
    } else if (cmd.rfind("shot", 0) == 0) {
        char path[256] = "sim/out/shot.png";
        std::sscanf(cmd.c_str(), "shot %255s", path);
        std::lock_guard<std::mutex> lock(g_fb_mutex);
        sim_display::lock();
        const bool ok = write_png_rgb565(path, sim_display::pixels(), W, H);
        sim_display::unlock();
        char extra[96];
        std::snprintf(extra, sizeof(extra), "ok %s frame=%u sim_ms=%u\n", path,
                      g_frames.load(), sim_clock::now_ms());
        reply = ok ? std::string(extra) : "error\n";
    } else if (cmd == "state") {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "sim_ms=%u frames=%u loops=%u last_loop_ms=%u lang=%s screen_off=%d app=%s voltage=%u level=%u %s\n",
                      sim_clock::now_ms(), g_frames.load(), g_loops.load(), g_last_loop_ms.load(),
                      i18nCode(i18nLang()),
                      GetHAL().screen_off_active() ? 1 : 0,
                      current_app_name().c_str(),
                      GetHAL().getBatteryVoltage(), GetHAL().getBatteryLevel(), mem_guard::report());
        reply = buf;
    } else if (cmd.rfind("log", 0) == 0) {
        int n = 40;
        std::sscanf(cmd.c_str(), "log %d", &n);
        reply = sim_log_tail(n);
    } else if (cmd.rfind("step", 0) == 0) {
        int ms = 100;
        std::sscanf(cmd.c_str(), "step %d", &ms);
        if (ms < 1) ms = 1;
        g_deterministic = true;
        sim_advance((uint32_t)ms);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "stepped to sim_ms=%u frame=%u\n",
                      sim_clock::now_ms(), g_frames.load());
        reply = buf;
    } else if (cmd == "pause") {
        g_deterministic = true;
        sim_clock::set_paused(true);
        reply = "paused\n";
    } else if (cmd == "resume") {
        g_deterministic = false;
        sim_clock::set_paused(false);
        reply = "resumed\n";
    } else if (cmd.rfind("clock", 0) == 0) {
        long long epoch = 0;
        int h = 0, m = 0, s2 = 0, y = 0, mo = 0, d = 0;
        if (std::sscanf(cmd.c_str(), "clock epoch %lld", &epoch) == 1) {
            sim_clock::set_epoch(epoch);
        } else if (std::sscanf(cmd.c_str(), "clock %d:%d:%d", &h, &m, &s2) == 3) {
            const int64_t base = sim_clock::epoch() - (sim_clock::epoch() % 86400);
            sim_clock::set_epoch(base + h * 3600 + m * 60 + s2);
        } else if (std::sscanf(cmd.c_str(), "clock date %d-%d-%d", &y, &mo, &d) == 3) {
            std::tm tm_buf{};
            tm_buf.tm_year = y - 1900;
            tm_buf.tm_mon  = mo - 1;
            tm_buf.tm_mday = d;
            sim_clock::set_epoch((int64_t)timegm(&tm_buf));
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "clock epoch=%lld\n", (long long)sim_clock::epoch());
        reply = buf;
    } else if (cmd.rfind("open ", 0) == 0) {
        /* open an app by name: deterministic, bypasses launcher gestures */
        const std::string name = cmd.substr(5);
        bool ok = false;
        for (auto& p : GetMooncake().getAllAppProps()) {
            if (p.info.name == name) {
                ok = GetMooncake().openApp(p.appID);
                break;
            }
        }
        reply = ok ? "ok\n" : "not found\n";
    } else if (cmd.rfind("closeapps", 0) == 0) {
        for (auto& p : GetMooncake().getAllAppProps()) GetMooncake().closeApp(p.appID);
        reply = "ok\n";
    } else if (cmd.rfind("combo", 0) == 0) {
        int ms = 600;
        std::sscanf(cmd.c_str(), "combo %d", &ms);
        sim_input::set_key('A', true);
        sim_input::set_key('B', true);
        sim_advance((uint32_t)ms);
        sim_input::set_key('A', false);
        sim_input::set_key('B', false);
        sim_advance(200);
        reply = "ok\n";
    } else if (cmd == "freeze") {
        g_frozen = true;          /* stop the main loop, used to test stall detection */
        reply = "frozen\n";
    } else if (cmd == "thaw") {
        g_frozen = false;
        reply = "ok\n";
    } else if (cmd == "crash") {
        std::fflush(stdout);
        std::abort();           /* deliberate crash, used to test the supervisor */
    } else if (cmd == "quit") {
        g_running = false;
        reply = "bye\n";
    } else if (cmd.rfind("lang", 0) == 0) {
        char code[32] = "";
        if (std::sscanf(cmd.c_str(), "lang %31s", code) == 1) {
            for (int i = 0; i < (int)Lang::COUNT; ++i) {
                if (std::strcmp(i18nCode((Lang)i), code) == 0) { i18nSetLang((Lang)i); break; }
            }
        } else {
            i18nSetLang((Lang)(((int)i18nLang() + 1) % (int)Lang::COUNT));
        }
        reply = std::string("lang=") + i18nCode(i18nLang()) + "\n";
    } else {
        reply = "unknown command\n";
    }
}

static void control_server()
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)ctrl_port());
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::perror("bind");
        return;
    }
    listen(srv, 4);
    std::printf("control listening on 127.0.0.1:%d\n", ctrl_port());
    while (g_running) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) break;
        char buf[512];
        ssize_t n = read(cli, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            std::string cmd(buf);
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
            std::string reply;
            handle_command(cmd, reply);
            (void)!write(cli, reply.data(), reply.size());
        }
        close(cli);
    }
    close(srv);
}

/* ------------------------------------------------------------------- main */
int main(int argc, char** argv)
{
    mem_guard::init();
    lv_init();
    lv_log_register_print_cb(log_cb);

    static std::vector<uint8_t> buf1(W * H * 2), buf2(W * H * 2);
    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    /* PARTIAL mode: px_map always starts at the area origin, which keeps the
       framebuffer copy exact (DIRECT mode gave offset artefacts). */
    lv_display_set_buffers(disp, buf1.data(), buf2.data(), (uint32_t)buf1.size(),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    lv_indev_set_display(indev, disp);
    GetHAL().lvTouchpad = indev;

    i18nInit();

    Mooncake& mc = GetMooncake();
    mc.installApp(std::make_unique<AppLauncher>());
    mc.installApp(std::make_unique<AppAlarmClock>());
    mc.installApp(std::make_unique<AppWatchFace>());
    mc.installApp(std::make_unique<AppStopWatch>());
    mc.installApp(std::make_unique<AppQiuqiu>());
    mc.installApp(std::make_unique<AppSetup>());

    std::thread server(control_server);

    /* optional: render one frame and exit (used for quick screenshots) */
    bool oneshot = (argc > 1 && std::strcmp(argv[1], "--shot") == 0);
    const char* out = argc > 2 ? argv[2] : "sim/out/state.png";

    const auto t0 = std::chrono::steady_clock::now();
    uint32_t tick_acc = 0;
    sim_clock::init();
    /* SIM_START_PAUSED=1 makes the run fully deterministic from t=0: nothing
       advances until a `step` command asks for it, so boot animations cannot
       depend on how long the process has been up. */
    if (std::getenv("SIM_START_PAUSED")) {
        g_deterministic = true;
        sim_clock::set_paused(true);
    }
    while (g_running) {
        /* deterministic mode: only advance when a `step` command asked for it */
        if (sim_clock::paused()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        uint32_t ms;
        if (g_deterministic) {
            ms = sim_clock::now_ms() + 5;
            sim_clock::advance(5);
            if (sim_clock::paused()) { /* budget exhausted by step handler */ }
        } else {
            const auto now = std::chrono::steady_clock::now();
            ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
            sim_clock::advance(ms - sim_clock::now_ms());
        }
        lv_tick_inc(ms - tick_acc);
        tick_acc = ms;

        if (g_frozen) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;              /* deliberately stop making progress */
        }
        g_loops++;
        g_last_loop_ms = ms;
        GetHAL().updateButtonStates();
        GetHAL().screenTick();
        mc.update();
        lv_timer_handler();

        if (oneshot) {
            lv_refr_now(nullptr);
            std::lock_guard<std::mutex> lock(g_fb_mutex);
            sim_display::lock();
            const bool ok = write_png_rgb565(out, sim_display::pixels(), W, H);
            sim_display::unlock();
            if (!ok) return 1;
            std::printf("wrote %s\n", out);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    g_running = false;
    server.join();
    return 0;
}
