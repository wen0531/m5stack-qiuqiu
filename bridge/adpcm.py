"""IMA-ADPCM (4-bit) codec, matching the firmware's implementation."""
import struct

STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,
876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,
4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,
22385,24623,27086,29794,32767]
IDXTAB = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8]


class Decoder:
    def __init__(self):
        self.pred = 0
        self.idx = 0

    def decode(self, data: bytes):
        out = []
        pred, idx = self.pred, self.idx
        for b in data:
            for nib in (b & 0xF, b >> 4):
                step = STEP[idx]
                diff = step >> 3
                if nib & 4: diff += step
                if nib & 2: diff += step >> 1
                if nib & 1: diff += step >> 2
                pred = pred - diff if (nib & 8) else pred + diff
                pred = max(-32768, min(32767, pred))
                idx = max(0, min(88, idx + IDXTAB[nib & 0x7]))
                out.append(pred)
        self.pred, self.idx = pred, idx
        return out


class Encoder:
    def __init__(self):
        self.pred = 0
        self.idx = 0

    def _nib(self, sample):
        step = STEP[self.idx]
        diff = sample - self.pred
        code = 0
        if diff < 0:
            code = 8; diff = -diff
        if diff >= step: code |= 4; diff -= step
        step >>= 2
        if diff >= step: code |= 2; diff -= step
        step >>= 1
        if diff >= step: code |= 1
        step = STEP[self.idx]
        vpdiff = step >> 3
        if code & 4: vpdiff += step
        if code & 2: vpdiff += step >> 1
        if code & 1: vpdiff += step >> 2
        self.pred = self.pred - vpdiff if (code & 8) else self.pred + vpdiff
        self.pred = max(-32768, min(32767, self.pred))
        self.idx = max(0, min(88, self.idx + IDXTAB[code]))
        return code

    def encode(self, pcm):
        out = bytearray()
        for i in range(0, len(pcm) - 1, 2):
            out.append((self._nib(pcm[i]) & 0xF) | ((self._nib(pcm[i + 1]) & 0xF) << 4))
        return bytes(out)
