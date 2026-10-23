/*
 * Voice link task — push-to-talk mic capture -> IMA-ADPCM -> BLE notify.
 * Reuses the ESP-IDF HAL audio path (ES8311 @ 44.1 kHz mono) and resamples
 * to 16 kHz for the wire format: 20 ms frame = 320 samples = 160 B ADPCM.
 */
#include "voice_state.h"

#include <cmath>
#include <cstring>
#include <vector>
#include <esp_heap_caps.h>

extern "C" {
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
}

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <hal/hal.h>

#include "adpcm.h"
#include "ble_svc.h"
#include "batt_log.h"

#include "apps/app_qiuqiu/qiuqiu_engine.h"
#include <vector>
#include <esp_heap_caps.h>

extern "C" {
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
}

static const char* TAG = "voice";
#define UPLINK_OPUS 0
static volatile bool g_mode16k = false;   /* codec currently in 16 kHz playback mode */
static volatile uint32_t g_last_pkt = 0;
static volatile int g_pending_mode = 0;   /* 1 = 16 kHz playback, 2 = back to 44.1 kHz */
static constexpr int CAPTURE_RATE = 44100;
static constexpr int TARGET_RATE = 16000;
static constexpr int FRAME_MS = 20;
static constexpr int IN_SAMPLES = CAPTURE_RATE * FRAME_MS / 1000;   /* 882 */
static constexpr int OUT_SAMPLES = TARGET_RATE * FRAME_MS / 1000;   /* 320 */
static constexpr int ADPCM_BYTES = OUT_SAMPLES / 2;                 /* 160 */
static constexpr int PKT_LEN = 4 + ADPCM_BYTES;                     /* 164 */

static constexpr gpio_num_t BTN_A = (gpio_num_t)2;
static constexpr gpio_num_t BTN_B = (gpio_num_t)1;
static bool btn_b_prev = false;   /* for edge detection: B cycles the backend */
static bool btn_a_prev = false;   /* for edge detection: A interrupts playback */

static volatile int s_state = VOICE_IDLE;
static volatile bool g_playing = false;
static bool s_session_active = false;   /* a turn holds the sleep gate */
/* A was pressed while a reply was playing: write_tick() drops the rest of the
   downlink (and drains the DMA with silence) until the next talk starts. */
static volatile bool g_play_interrupt = false;
static int fade_in_left = 0;   /* start-of-playback gain ramp (samples) */
static int saved_spk_vol = -1; /* speaker volume before we clamped it for TTS */
static uint32_t last_data_ms = 0;   /* millis of the last block fed to the codec */
static volatile bool s_ptt_enabled = false;

extern "C" void voice_set_ptt_enabled(bool enabled) { s_ptt_enabled = enabled; }
extern "C" bool voice_get_ptt_enabled(void) { return s_ptt_enabled; }
static volatile bool s_connected = false;
static volatile bool s_ever_connected = false;

voice_state_t voice_state_get(void) { return (voice_state_t)s_state; }
bool voice_state_connected(void) { return s_connected; }
bool voice_state_ever_connected(void) { return s_ever_connected; }


static void play_tone_test(bool via_hal_play);
static void play_long_tone(void);
static void interrupt_playback(void);
namespace { void ring_flush(); }   /* defined with the downlink ring further below */

/* ---- uplink Opus encoder (16 kHz mono, 60 ms, 24 kbps) ---- */
static void* g_opus_enc = nullptr;
static int g_enc_in_size = 1920;
static uint8_t* g_enc_out = nullptr;
static uint8_t* g_enc_pkt = nullptr;
static int g_enc_out_size = 2048;
static bool opus_encoder_ready()
{
    if (g_opus_enc != nullptr) return true;
    esp_opus_enc_config_t cfg = {};
    cfg.sample_rate      = 16000;
    cfg.channel          = 1;
    cfg.bits_per_sample  = 16;
    cfg.bitrate          = 16000;   /* frames stay well under the 247-byte MTU */
    cfg.frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity       = 5;
    cfg.enable_fec       = false;
    cfg.enable_dtx       = false;
    cfg.enable_vbr       = true;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &g_opus_enc) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus encoder open failed");
        g_opus_enc = nullptr;
        return false;
    }
    int in_size = 0, out_size = 0;
    esp_opus_enc_get_frame_size(g_opus_enc, &in_size, &out_size);
    g_enc_in_size = in_size;
    g_enc_out_size = out_size;
    ESP_LOGI(TAG, "opus encoder ready (16 kHz mono, 60 ms, 16 kbps) in=%d out=%d", in_size, out_size);
    return true;
}
#if 0
#endif

/* A long-press (>500 ms) is push-to-talk; a short tap stays an expression
   change (handled by the app's KeyManager). B never talks. */
static constexpr uint32_t PTT_LONG_MS = 500;

/* Set when A interrupted playback: that press must not become a PTT, so it is
   swallowed until the key is released again. */
static bool s_ptt_suppressed = false;

static bool ptt_pressed(uint32_t now)
{
    static bool held = false;
    static uint32_t pressed_at = 0;

    const bool down = s_ptt_enabled && (gpio_get_level(BTN_A) == 0);
    if (s_ptt_suppressed) {
        if (!down) s_ptt_suppressed = false;   /* re-arm on release */
        held = false;
        return false;
    }
    if (down) {
        if (!held) {
            held = true;
            pressed_at = now;
        }
        return (now - pressed_at) >= PTT_LONG_MS;
    }
    held = false;
    return false;
}

static void voice_task(void* arg)
{
    (void)arg;

    std::vector<int16_t> in;
    in.reserve(IN_SAMPLES);
    static int16_t pcm16[OUT_SAMPLES];
    static uint8_t adpcm[ADPCM_BYTES];
    static uint8_t pkt[PKT_LEN];
    static int16_t* acc = (int16_t*)heap_caps_malloc(960 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int acc_n = 0;
    uint32_t tx_bytes = 0;

    adpcm_state_t st;
    adpcm_reset(&st);

    uint16_t seq = 0;
    bool talking = false;
    bool was_connected = false;
    uint32_t happy_until = 0;
    uint32_t think_until = 0;
    uint32_t frames = 0;

    for (;;) {
        bool connected = ble_svc_ready();
        uint32_t now = GetHAL().millis();
        s_connected = connected;

        if (connected && !was_connected) {
            s_ever_connected = true;
            happy_until = now + 1500;
            ESP_LOGI(TAG, "BLE connected");
        } else if (!connected && was_connected) {
            think_until = 0;
            ESP_LOGI(TAG, "BLE disconnected");
        }
        was_connected = connected;

        /* B press (only inside the Qiuqiu app) asks the phone to cycle backend */
        {
            const bool b_down = (gpio_get_level(BTN_B) == 0);
            if (b_down && !btn_b_prev && s_ptt_enabled) {
                /* instant local feedback: the ball changes colour before the
                   network round-trip completes */
                const int idx = qiuqiu::cycle_agent_color();
                ESP_LOGI(TAG, "B key -> cycle backend (colour %d)", idx);
                ble_svc_event_send(0x20);
            }
            btn_b_prev = b_down;
        }

        /* A down edge while a reply is playing means "stop talking". It takes
           priority over push-to-talk: the press is swallowed so it cannot
           start a new capture, even when it is held down. */
        {
            const bool a_down = (gpio_get_level(BTN_A) == 0);
            if (a_down && !btn_a_prev && g_playing) {
                interrupt_playback();
                s_ptt_suppressed = true;   /* this press must not become a PTT */
                think_until = 0;           /* interrupted: straight back to idle */
                ESP_LOGI(TAG, "A key -> playback interrupted");
            }
            btn_a_prev = a_down;
        }

        /* keep the panel awake while talking, waiting for the agent, or playing */
        if (talking || g_playing || (think_until != 0 && (int32_t)(think_until - now) > 0)) {
            GetHAL().markActivity();
        }
        GetHAL().audioIdleCheck();

        /* stream on local PTT or when the PC requests talk via control 0x01 */
        GetHAL().audioIdleCheck();   /* close the codec after 30 s of silence */
        bool want = (ptt_pressed(now) || ble_svc_remote_talk()) && connected;
        if (want && !talking) {
            if (!s_session_active) { s_session_active = true; GetHAL().powerBusy("session"); }
            adpcm_reset(&st);
            seq = 0;
            talking = true;
            /* a new turn re-enables the downlink after an interruption and
               throws away whatever the phone had already queued */
            if (g_play_interrupt) {
                g_play_interrupt = false;
                ring_flush();
            }
            ble_svc_set_audio_mode(true);
            ble_svc_status_notify(0x10);
            ESP_LOGI(TAG, "PTT start");
        } else if (!want && talking) {
            talking = false;
            think_until = now + 30000;   /* keep the panel awake while the agent answers */
            /* the gate stays held until the answer has played (or timed out) */
            if (!g_playing) ble_svc_set_audio_mode(false);
            ble_svc_status_notify(0x11);
            ESP_LOGI(TAG, "PTT stop (frames=%lu)", (unsigned long)frames);
        }

        if (!talking) {
            uint8_t cmd, val;
            while (ble_svc_take_command(&cmd, &val)) {
                if (cmd == 0x31) play_tone_test(true);
                else if (cmd == 0x32) play_tone_test(false);
                else if (cmd == 0x33) GetHAL().playBootSfx();
                else if (cmd == 0x34) play_long_tone();
                else if (cmd == 0x50) qiuqiu::set_agent_color((int)val);
                else if (cmd == 0x40) batt_log_dump();
                else if (cmd == 0x36) { /* codec stays 44.1 kHz: no mode switch */ }
                else if (cmd == 0x37) { }
            }
            if (s_session_active && !talking && !g_playing &&
                (int32_t)(now - think_until) > 0) {
                s_session_active = false;
                GetHAL().powerIdle("session");
            }
            if (g_playing && !g_play_interrupt) {
                s_state = VOICE_SPEAKING;
            } else if (!connected) {
                s_state = VOICE_SLEEPY;
            } else if (now < happy_until) {
                s_state = VOICE_HAPPY;
            } else if (now < think_until) {
                s_state = VOICE_THINKING;
            } else {
                s_state = VOICE_IDLE;
            }
            vTaskDelay(pdMS_TO_TICKS(30));   /* idle: 30 ms keeps quick B-key presses */
            continue;
        }

        s_state = VOICE_LISTENING;
        GetHAL().audioRecord(in, FRAME_MS, 30.0f);
        if ((int)in.size() < IN_SAMPLES) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        /* 44.1 kHz -> 16 kHz, linear interpolation */
        const float step = (float)(IN_SAMPLES - 1) / (float)(OUT_SAMPLES - 1);
        for (int i = 0; i < OUT_SAMPLES; i++) {
            float pos = (float)i * step;
            int i0 = (int)pos;
            if (i0 > IN_SAMPLES - 2) i0 = IN_SAMPLES - 2;
            float f = pos - (float)i0;
            pcm16[i] = (int16_t)((1.0f - f) * (float)in[i0] + f * (float)in[i0 + 1]);
        }

#if UPLINK_OPUS
        /* uplink: accumulate 60 ms, encode one Opus frame */
        for (int i = 0; i < OUT_SAMPLES; i++) {
            if (acc_n < 960) acc[acc_n++] = pcm16[i];
        }
        if (acc_n >= 960) {
            if (opus_encoder_ready()) {
                if (g_enc_out == nullptr) {
                    g_enc_out = (uint8_t*)heap_caps_malloc((size_t)g_enc_out_size, MALLOC_CAP_SPIRAM);
                    g_enc_pkt = (uint8_t*)heap_caps_malloc((size_t)g_enc_out_size + 4, MALLOC_CAP_SPIRAM);
                    if (g_enc_out == nullptr || g_enc_pkt == nullptr) {
                        ESP_LOGE(TAG, "opus buffers alloc failed");
                        vTaskDelete(NULL);
                        return;
                    }
                }
                esp_audio_enc_in_frame_t in_frame = {};
                in_frame.buffer = (uint8_t*)acc;
                in_frame.len    = (uint32_t)(960 * 2);
                esp_audio_enc_out_frame_t out_frame = {};
                out_frame.buffer = g_enc_out;
                out_frame.len    = (uint32_t)g_enc_out_size;
                esp_audio_err_t erc = esp_opus_enc_process(g_opus_enc, &in_frame, &out_frame);
                static int dbg = 0;
                if ((dbg++ % 20) == 0) {
                    ESP_LOGI(TAG, "enc rc=%d bytes=%u", (int)erc, (unsigned)out_frame.encoded_bytes);
                }
                if (erc == ESP_AUDIO_ERR_OK && out_frame.encoded_bytes > 0) {
                    g_enc_pkt[0] = (uint8_t)(seq & 0xFF);
                    g_enc_pkt[1] = (uint8_t)(seq >> 8);
                    g_enc_pkt[2] = 2;
                    g_enc_pkt[3] = 0;
                    memcpy(g_enc_pkt + 4, g_enc_out, out_frame.encoded_bytes);
                    ble_svc_send_audio(g_enc_pkt, 4 + out_frame.encoded_bytes);
                    seq++;
                    frames++;
                    tx_bytes += out_frame.encoded_bytes;
                }
            }
            acc_n = 0;
        }
#else
        /* uplink: 16 kHz PCM -> IMA-ADPCM (64 kbps, proven path) */
        adpcm_encode_block(&st, pcm16, OUT_SAMPLES, adpcm);
        pkt[0] = (uint8_t)(seq & 0xFF);
        pkt[1] = (uint8_t)(seq >> 8);
        pkt[2] = 1;
        pkt[3] = 0;
        memcpy(pkt + 4, adpcm, ADPCM_BYTES);
        ble_svc_send_audio(pkt, PKT_LEN);
        seq++;
        frames++;
#endif
        if (frames > 0 && (frames % 50) == 0) {
            ESP_LOGI(TAG, "tx=%lu frames err=%lu bytes=%lu", (unsigned long)ble_svc_tx_packets(),
                     (unsigned long)ble_svc_tx_errors(), (unsigned long)tx_bytes);
        }
    }
}

/* Diagnostic tones: 0x31 plays through HAL::audioPlay (the same path the
 * factory UI uses), 0x32 plays through our streaming writer. */
static void play_long_tone(void)
{
    constexpr int RATE = 44100;
    constexpr int N = RATE * 4 / 5;      /* 0.8 s */
    std::vector<int16_t> v((size_t)N);
    for (int i = 0; i < N; i++) {
        float env = 1.0f;
        if (i < 200) env = (float)i / 200.0f;
        if (N - i < 400) env = (float)(N - i) / 400.0f;
        v[(size_t)i] = (int16_t)(25000.0f * env * sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)RATE));
    }
    GetHAL().audioPlay(v, true);
    ESP_LOGI(TAG, "long tone 440Hz 0.8s");
}

static void play_tone_test(bool via_hal_play)
{
    constexpr int RATE = 44100;
    constexpr float AMP = 2000.0f;
    constexpr int TOTAL = RATE * 2 / 5;   /* 0.4 s */
    const float FREQ = via_hal_play ? 1000.0f : 1500.0f;  /* distinguishable */
    if (via_hal_play) {
        std::vector<int16_t> v((size_t)TOTAL);
        for (int i = 0; i < TOTAL; i++) {
            v[(size_t)i] = (int16_t)(AMP * sinf(2.0f * 3.14159265f * FREQ * (float)i / (float)RATE));
        }
        GetHAL().audioPlay(v, true);
        ESP_LOGI(TAG, "tone test: HAL audioPlay path (%d samples)", TOTAL);
    } else {
        int16_t chunk[RATE / 100];
        TickType_t last = xTaskGetTickCount();
        for (int off = 0; off + (RATE / 100) <= TOTAL; off += (RATE / 100)) {
            for (int i = 0; i < RATE / 100; i++) {
                chunk[i] = (int16_t)(AMP * sinf(2.0f * 3.14159265f * FREQ * (float)(off + i) / (float)RATE));
            }
            GetHAL().audioWriteStream(chunk, RATE / 100);
            vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
        }
        ESP_LOGI(TAG, "tone test: writeStream path (%d samples)", TOTAL);
    }
}

/* PC -> device audio path:
 *   BLE packets -> ADPCM decode (16 kHz) -> resample 44.1 kHz -> ring buffer
 *   -> a steady 10 ms writer feeds the codec.
 * Decoupling the two sides removes the crackle caused by BLE jitter. */
/* PC -> device audio: accumulate 0.3 s of 16 kHz ADPCM-decoded audio, resample
 * it once to 44.1 kHz and hand it to the proven HAL playback path. */
namespace {
constexpr int DL_IN_RATE = 16000;
constexpr int DL_OUT_RATE = 44100;
constexpr int DL_BLOCK = DL_IN_RATE / 2;            /* 0.5 s of 16 kHz */
int16_t* g_dl_block = nullptr;
int g_dl_fill = 0;
}

static void* g_opus_dec = nullptr;

static bool opus_decoder_ready()
{
    if (g_opus_dec != nullptr) return true;
    esp_opus_dec_cfg_t cfg = {};
    cfg.sample_rate    = 16000;
    cfg.channel        = ESP_AUDIO_MONO;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    cfg.self_delimited = false;
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &g_opus_dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus decoder open failed");
        g_opus_dec = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "opus decoder ready (16 kHz mono, 60 ms)");
    return true;
}

static int opus_decode_frame(const uint8_t* data, int len, int16_t* out, int max_samples)
{
    if (!opus_decoder_ready()) return 0;
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t*>(data);
    raw.len = (uint32_t)len;
    esp_audio_dec_out_frame_t frame = {};
    frame.buffer = (uint8_t*)out;
    frame.len = (uint32_t)max_samples * 2;
    esp_audio_dec_info_t info = {};
    esp_audio_err_t err = esp_opus_dec_decode(g_opus_dec, &raw, &frame, &info);
    if (err != ESP_AUDIO_ERR_OK) {
        return 0;
    }
    /* frame.len is the CAPACITY we passed in; decoded_size is what was written.
       Using len here pushed 1440 uninitialised samples of every 60 ms frame
       into the ring, which is what produced the old "buzz". */
    size_t got = frame.decoded_size / 2;
    if (got == 0 || got > (size_t)max_samples) {
        return 0;
    }
    return (int)got;
}

/* Audio downlink pipeline:
 *   BLE -> Opus decode (16 kHz) -> PCM ring -> writer task (20 ms ticks)
 *   The codec is switched to 16 kHz mono output by command 0x36 and back by
 *   0x37, so the samples go straight to the DAC with no resampling.          */
namespace {
constexpr int IN_RATE  = 16000;
constexpr int OUT_RATE = 44100;
constexpr int RING_SAMPLES = IN_RATE * 2;     /* 2 s at 16 kHz */
constexpr int TICK_SAMPLES = IN_RATE / 100;   /* 10 ms at 16 kHz */
int16_t* g_ring = nullptr;
int g_head = 0, g_tail = 0;
std::mutex g_ring_mutex;

inline int ring_avail_locked()
{
    int n = g_head - g_tail;
    if (n < 0) n += RING_SAMPLES;
    return n;
}

void ring_push(const int16_t* d, int n)
{
    std::lock_guard<std::mutex> lock(g_ring_mutex);
    for (int i = 0; i < n; i++) {
        int next = (g_head + 1) % RING_SAMPLES;
        if (next == g_tail) break;               /* full: drop */
        g_ring[g_head] = d[i];
        g_head = next;
    }
}

void ring_flush()
{
    std::lock_guard<std::mutex> lock(g_ring_mutex);
    g_head = g_tail = 0;
}

/* Soft-knee limiter: linear below 0.78 FS, compresses smoothly above it. */
static inline float soft_limit(float v)
{
    constexpr float knee = 29500.0f;
    constexpr float head = 32767.0f - knee;
    const float a = v < 0.0f ? -v : v;
    if (a <= knee) return v;
    const float over = a - knee;
    const float lim = knee + head * (1.0f - expf(-over / head));
    return v < 0.0f ? -lim : lim;
}

void write_tick()
{
    /* Standard BLE-audio writer: a fixed 20 ms cadence that matches the codec
       frame size, a 200 ms pre-buffer before the first block, and silence (not
       a stall) when the ring underruns. Fixed sizes only - no adaptive tuning. */
    constexpr int IN_BLOCK  = IN_RATE / 50;            /* 20 ms = 320 samples @16k */
    constexpr int OUT_BLOCK = (int)((long)IN_BLOCK * OUT_RATE / IN_RATE);  /* 882 @44.1k */
    constexpr int PREBUFFER = IN_RATE / 5;             /* 200 ms before starting */

    static bool  started = false;
    static int   underrun_ms = 0;
    static std::vector<int16_t> out;

    /* interrupted playback: stop feeding audio, push a short silence tail */
    static int abort_silence = 0;
    if (g_play_interrupt) {
        if (abort_silence < 2) {
            static std::vector<int16_t> zeros(OUT_RATE / 5, 0);
            GetHAL().audioWriteStream(zeros.data(), zeros.size());
            abort_silence++;
        }
        g_playing = false;
        GetHAL().powerIdle("audio");
        if (s_session_active) {           /* interrupted: the turn is over */
            s_session_active = false;
            GetHAL().powerIdle("session");
        }
        last_data_ms = 0;
        started = false;
        return;
    }
    abort_silence = 0;

    static int16_t in[IN_BLOCK];
    int got = 0;
    {
        std::lock_guard<std::mutex> lock(g_ring_mutex);
        const int avail = ring_avail_locked();
        if (!started) {
            if (avail < PREBUFFER) {
                return;                     /* keep buffering before the first block */
            }
            started = true;
            g_playing = true;
            GetHAL().powerBusy("audio");
            fade_in_left = 882;             /* 20 ms fade-in */
            ble_svc_set_audio_mode(true);
            ble_svc_status_notify(0x12);
            saved_spk_vol = GetHAL().getSpeakerVolume();
        }
        got = (avail < IN_BLOCK) ? avail : IN_BLOCK;
        for (int i = 0; i < got; i++) {
            in[i] = g_ring[g_tail];
            g_tail = (g_tail + 1) % RING_SAMPLES;
        }
    }
    if (got < IN_BLOCK) {
        memset(in + got, 0, (size_t)(IN_BLOCK - got) * sizeof(int16_t));
        underrun_ms += 20;
    } else {
        underrun_ms = 0;
    }

    out.assign((size_t)OUT_BLOCK, 0);
    for (int i = 0; i < OUT_BLOCK; i++) {
        float pos = (float)i * (float)IN_RATE / (float)OUT_RATE;
        int i0 = (int)pos;
        if (i0 > IN_BLOCK - 2) i0 = IN_BLOCK - 2;
        float f = pos - (float)i0;
        float v = (1.0f - f) * (float)in[i0] + f * (float)in[i0 + 1];
        if (fade_in_left > 0) {
            fade_in_left--;
            v *= (float)(882 - fade_in_left) / 882.0f;
        }
        out[(size_t)i] = (int16_t)soft_limit(v);
    }
    GetHAL().audioWriteStream(out.data(), out.size());
    last_data_ms = GetHAL().millis();

    /* 2 s without data: the clip is over */
    if (underrun_ms > 2000) {
        started = false;
        underrun_ms = 0;
        g_playing = false;
        if (saved_spk_vol >= 0) { GetHAL().setSpeakerVolume(saved_spk_vol); saved_spk_vol = -1; }
        ble_svc_set_audio_mode(false);
        ble_svc_status_notify(0x13);
        last_data_ms = 0;
    }
}


}  // namespace

/* Stop an in-flight reply from the voice task: drop the queued downlink,
   mute write_tick() (it feeds a short silence tail so the DMA drains without
   a pop) and tell the phone that playback ended. */
static void interrupt_playback(void)
{
    ring_flush();
    g_play_interrupt = true;
    g_playing = false;
    if (saved_spk_vol >= 0) {
        GetHAL().setSpeakerVolume(saved_spk_vol);
        saved_spk_vol = -1;
    }
    ble_svc_set_audio_mode(false);
    ble_svc_status_notify(0x13);
    ble_svc_event_send(0x13);   /* reliable mirror on the audio characteristic */
}

static void downlink_writer_task(void* arg)
{
    (void)arg;
    for (;;) {
        write_tick();
        vTaskDelay(pdMS_TO_TICKS(20));   /* idle downlink poll (20 ms: 60 ms causes underruns) */
    }
}

static void downlink_task(void* arg)
{
    (void)arg;
    adpcm_state_t st;
    adpcm_reset(&st);
    static int16_t* inbuf = (int16_t*)heap_caps_calloc(2400, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!inbuf) { ESP_LOGE(TAG, "inbuf alloc failed"); vTaskDelete(NULL); return; }

    g_ring = (int16_t*)heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!g_ring) {
        ESP_LOGE(TAG, "downlink ring alloc failed");
        vTaskDelete(NULL);
        return;
    }
    g_head = g_tail = 0;

    for (;;) {
        uint8_t pkt[512];
        size_t len = 0;
        if (!ble_svc_take_downlink(pkt, sizeof(pkt), &len) || len < 6) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (pkt[2] != 0 && pkt[2] != 1 && pkt[2] != 2) continue;
        /* playback was interrupted: swallow late reply frames until the next
           talk starts so the ring cannot refill behind the mute */
        if (g_play_interrupt) continue;
        int nsamp = 0;
        if (pkt[2] == 0) {
            /* raw PCM16 test path: no codec at all */
            nsamp = ((int)len - 4) / 2;
            if (nsamp <= 0 || nsamp > 2400) continue;   /* inbuf holds 2400 samples */
            memcpy(inbuf, pkt + 4, (size_t)nsamp * 2);
        } else if (pkt[2] == 1) {
            nsamp = ((int)len - 4) * 2;
            if (nsamp > 2400) continue;
            adpcm_decode_block(&st, pkt + 4, nsamp, inbuf);
        } else {
            if (g_last_pkt == 0 || (GetHAL().millis() - g_last_pkt) > 500) {
                esp_opus_dec_reset(g_opus_dec);   /* new clip: clear decoder state */
            }
            nsamp = opus_decode_frame(pkt + 4, (int)len - 4, inbuf, 2400);
            if (nsamp <= 0) continue;
        }
        g_last_pkt = GetHAL().millis();
        ring_push(inbuf, nsamp);
    }
}

extern "C" void voice_start(void)
{
    // BLE is an optional feature: never let a radio failure abort the boot.
    esp_err_t ret = ble_svc_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed (%d) - Qiuqiu and factory apps keep running", ret);
        return;
    }
    batt_log_start();
    if (xTaskCreate(voice_task, "voice", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "voice task create failed");
    }
    if (xTaskCreate(downlink_writer_task, "dl_writer", 4096, NULL, 7, NULL) != pdPASS) {
        ESP_LOGE(TAG, "downlink writer create failed");
    }
    /* Opus decoding needs a deep stack; keep it in PSRAM so internal RAM stays free. */
    static StaticTask_t dl_tcb;
    static StackType_t* dl_stack = nullptr;
    if (dl_stack == nullptr) {
        dl_stack = (StackType_t*)heap_caps_malloc(24 * 1024, MALLOC_CAP_SPIRAM);
    }
    if (dl_stack == nullptr ||
        xTaskCreateStatic(downlink_task, "dl_audio", 24 * 1024 / sizeof(StackType_t), NULL, 6, dl_stack, &dl_tcb) == NULL) {
        ESP_LOGE(TAG, "downlink task create failed");
    }

}
