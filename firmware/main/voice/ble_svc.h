/*
 * StopWatch BLE voice M1 — custom GATT service (NimBLE peripheral)
 *
 * Protocol (v1, M1):
 *   Service 4f5a0001-1e3b-4d8a-9c7e-5a1b2c3d4e5f
 *   Control 4f5a0002-... : WRITE  [0x01 = talk on, 0x00 = talk off, 0x02 = ping]
 *   Audio   4f5a0003-... : NOTIFY 20 ms packet:
 *                          u16 seq (LE) | u8 codec (1=IMA-ADPCM 4bit) | u8 flags | 160 B payload
 *   Status  4f5a0004-... : READ   [connected, subscribed, talking, pkts(4B LE), errs]
 *
 * Audio: mono 16 kHz, 20 ms = 320 samples -> 160 bytes ADPCM (8 kB/s).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SW_BLE_CTRL_TALK_ON  0x01
#define SW_BLE_CTRL_TALK_OFF 0x00
#define SW_BLE_CTRL_PING     0x02

esp_err_t ble_svc_start(void);

/* connected and audio notifications subscribed */
bool ble_svc_ready(void);

/* last talk state requested over the control characteristic */
bool ble_svc_remote_talk(void);

/* send one audio packet (164 bytes); drops if MTU too small / not subscribed */
void ble_svc_send_audio(const uint8_t *pkt, size_t len);

/* Debug: push an arbitrary blob to the PC over the audio characteristic using
 * codec id 0x7F; the payload is split into MTU-sized notifications. */
void ble_svc_debug_send(const uint8_t *data, size_t len);

/* Voice events pushed on the status characteristic (requires notify subscribe):
 * 0x10 talk start, 0x11 talk stop, 0x12 playback start, 0x13 playback end. */
void ble_svc_status_notify(uint8_t ev);

/* Reliable side-channel on the audio characteristic (codec 0x7E). */
void ble_svc_event_send(uint8_t ev);

/* true = short interval for audio, false = long interval + latency for idle */
void ble_svc_set_audio_mode(bool fast);

/* PC -> device audio downlink: the audio characteristic also accepts writes
 * (same packet layout). Frames are queued for the playback task. */
bool ble_svc_take_downlink(uint8_t *pkt, size_t max_len, size_t *out_len);

/* monotic activity counter: bumps on connect/subscribe/control writes */
uint32_t ble_svc_activity(void);

/* PC -> device commands queued by the control characteristic.
 *   0x10 <brightness 0..255>
 *   0x20 <expression id 0..14>
 */
bool ble_svc_take_command(uint8_t *cmd, uint8_t *value);
void ble_svc_requeue_command(uint8_t cmd, uint8_t value);

uint32_t ble_svc_tx_packets(void);
uint32_t ble_svc_tx_errors(void);

#ifdef __cplusplus
}
#endif
