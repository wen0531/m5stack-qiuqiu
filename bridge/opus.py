"""Minimal Opus encoder/decoder wrapper (ctypes) for the watch downlink.

16 kHz mono, 60 ms frames, 32 kbps CBR -> 240-byte payloads.
"""
import ctypes
import ctypes.util

_lib = ctypes.CDLL(ctypes.util.find_library('opus') or 'libopus.so.0')

OPUS_APPLICATION_VOIP = 2048
OPUS_SET_BITRATE_REQUEST = 4002
OPUS_SET_COMPLEXITY_REQUEST = 4010
OPUS_SET_VBR_REQUEST = 4006
OPUS_SET_INBAND_FEC_REQUEST = 4012
OPUS_SET_SIGNAL_REQUEST = 4024
OPUS_SIGNAL_VOICE = 3001
OPUS_RESET_STATE = 4028

_lib.opus_encoder_create.restype = ctypes.c_void_p
_lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
_lib.opus_encode.restype = ctypes.c_int32
_lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                             ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int32]
_lib.opus_encoder_ctl.restype = ctypes.c_int
_lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int]   # variadic
_lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]

_lib.opus_decoder_create.restype = ctypes.c_void_p
_lib.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
_lib.opus_decode.restype = ctypes.c_int
_lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int32,
                             ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
_lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]


def _ctl(enc, request, value):
    return _lib.opus_encoder_ctl(enc, request, ctypes.c_int(value))


class Encoder:
    def __init__(self, fs=16000, channels=1, bitrate=32000, complexity=5, frame_ms=60):
        err = ctypes.c_int(0)
        self.fs = fs
        self.frame_ms = frame_ms
        self.frame_size = fs * frame_ms // 1000
        self.enc = _lib.opus_encoder_create(fs, channels, OPUS_APPLICATION_VOIP, ctypes.byref(err))
        if err.value != 0:
            raise RuntimeError(f'opus_encoder_create failed: {err.value}')
        _ctl(self.enc, OPUS_SET_BITRATE_REQUEST, bitrate)
        _ctl(self.enc, OPUS_SET_COMPLEXITY_REQUEST, complexity)
        _ctl(self.enc, OPUS_SET_SIGNAL_REQUEST, OPUS_SIGNAL_VOICE)
        _ctl(self.enc, OPUS_SET_VBR_REQUEST, 0)   # CBR: constant packet size for BLE
        _ctl(self.enc, OPUS_SET_INBAND_FEC_REQUEST, 0)
        # worst case for CBR 32k/60ms is ~240 B; give slack for VBR peaks
        self.max_payload = 512

    def encode(self, pcm):
        """pcm: list[int] of exactly frame_size samples."""
        n = len(pcm)
        if n < self.frame_size:
            pcm = list(pcm) + [0] * (self.frame_size - n)
        elif n > self.frame_size:
            pcm = pcm[:self.frame_size]
        arr = (ctypes.c_int16 * self.frame_size)(*pcm)
        buf = (ctypes.c_ubyte * self.max_payload)()
        got = _lib.opus_encode(self.enc, arr, self.frame_size, buf, self.max_payload)
        if got < 0:
            raise RuntimeError(f'opus_encode failed: {got}')
        return bytes(buf[:got])

    def reset(self):
        _ctl(self.enc, OPUS_RESET_STATE, 0)

    def close(self):
        if getattr(self, 'enc', None):
            _lib.opus_encoder_destroy(self.enc)
            self.enc = None


class Decoder:
    def __init__(self, fs=16000, channels=1):
        err = ctypes.c_int(0)
        self.dec = _lib.opus_decoder_create(fs, channels, ctypes.byref(err))
        if err.value != 0:
            raise RuntimeError(f'opus_decoder_create failed: {err.value}')
        self.fs = fs

    def decode(self, data, samples=960):
        buf = (ctypes.c_int16 * samples)()
        arr = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        got = _lib.opus_decode(self.dec, arr, len(data), buf, samples, 0)
        if got < 0:
            raise RuntimeError(f'opus_decode failed: {got}')
        return list(buf[:got])

    def close(self):
        if getattr(self, 'dec', None):
            _lib.opus_decoder_destroy(self.dec)
            self.dec = None
