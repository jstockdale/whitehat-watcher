#!/usr/bin/env python3
"""IMA-ADPCM decoder matching the Watcher PTT firmware encoder.

The firmware (components/ptt/ptt.c) encodes 16kHz 16-bit mono PCM with
standard IMA-ADPCM: predictor starts at 0, step_index starts at 0,
low nibble first then high nibble per byte.
"""

import struct

STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
    37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8,
               -1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_decode(data: bytes) -> bytes:
    """Decode IMA-ADPCM bytes to 16-bit little-endian PCM."""
    predictor = 0
    step_index = 0
    out = bytearray()
    for byte in data:
        for nibble in (byte & 0x0F, (byte >> 4) & 0x0F):
            step = STEP_TABLE[step_index]
            diffq = step >> 3
            if nibble & 4:
                diffq += step
            if nibble & 2:
                diffq += step >> 1
            if nibble & 1:
                diffq += step >> 2
            if nibble & 8:
                predictor -= diffq
            else:
                predictor += diffq
            if predictor > 32767:
                predictor = 32767
            elif predictor < -32768:
                predictor = -32768
            out += struct.pack('<h', predictor)
            step_index += INDEX_TABLE[nibble]
            if step_index < 0:
                step_index = 0
            elif step_index > 88:
                step_index = 88
    return bytes(out)


def selftest() -> bool:
    """Round-trip sanity: decode must mirror the encoder's state machine."""
    # A few hand-computed vectors from the C encoder logic:
    # silence (0x00 bytes) -> predictor stays 0
    pcm = adpcm_decode(bytes([0x00, 0x00]))
    assert pcm == b'\x00\x00' * 4, pcm.hex()
    # 0x77: nibble 7 -> small positive step; just check it runs and is sane
    pcm = adpcm_decode(bytes([0x77]))
    assert len(pcm) == 4
    print("adpcm selftest OK")
    return True


if __name__ == "__main__":
    selftest()
