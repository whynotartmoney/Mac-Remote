"""PC Voice link protocol: ADPCM frames and the host handshake.

The device notifies audio on the event characteristic. The PC writes the
hello, the transcript, and failure on the text characteristic.
"""

from __future__ import annotations

import sys

SERVICE_UUID = "f7a1c3e0-5b24-4d91-8c6e-1a2b3c4d5e6f"
EVENT_UUID = "f7a1c3e0-5b24-4d91-8c6e-1a2b3c4d5e70"
TEXT_UUID = "f7a1c3e0-5b24-4d91-8c6e-1a2b3c4d5e71"
DEVICE_NAME = "PcVoice"

FRAME_AUDIO_START = 0x02
FRAME_AUDIO = 0x03
FRAME_AUDIO_END = 0x04
FRAME_DELETE = 0x05
FRAME_SEND = 0x06
FRAME_CLEAR = 0x07
END_FAIL = 0x02

HELLO = bytes((0x01, ord("P"), ord("V"), 0x01))
FAIL = bytes((0x11,))
TEXT_LIMIT = 480

# Windows speech LANGIDs, preferred the same way the Mac host preferred locales.
LANG_CANTONESE = "0c04"
LANG_TRADITIONAL = "0404"
LANG_MANDARIN = "0804"
LANG_ENGLISH = "0409"
LANG_ORDER = (LANG_CANTONESE, LANG_TRADITIONAL, LANG_MANDARIN, LANG_ENGLISH)

_STEPS = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)
_ADJUST = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)


def _clamp(value: int) -> int:
    return min(32767, max(-32768, value))


def encode(samples: list[int]) -> bytes:
    predictor = 0
    index = 0
    out = bytearray(len(samples) // 2)
    for i, sample in enumerate(samples):
        nibble = _encode_nibble(predictor, index, sample)
        predictor, index, nibble = nibble
        if i % 2 == 0:
            out[i // 2] = nibble
        else:
            out[i // 2] |= nibble << 4
    return bytes(out)


def _encode_nibble(predictor: int, index: int, sample: int) -> tuple[int, int, int]:
    diff = sample - predictor
    nibble = 0
    if diff < 0:
        nibble = 8
        diff = -diff
    step_index = min(88, max(0, index))
    step = _STEPS[step_index]
    delta = step >> 3
    if diff >= step:
        nibble |= 4
        diff -= step
        delta += step
    step >>= 1
    if diff >= step:
        nibble |= 2
        diff -= step
        delta += step
    step >>= 1
    if diff >= step:
        nibble |= 1
        delta += step
    predictor = _clamp(predictor + (-delta if (nibble & 8) else delta))
    step_index = min(88, max(0, step_index + _ADJUST[nibble & 0x0F]))
    return predictor, step_index, nibble & 0x0F


def decode(predictor: int, index: int, nibbles: bytes, samples: int) -> list[int]:
    pcm: list[int] = []
    pred = predictor
    idx = index
    for i in range(samples):
        byte = nibbles[i // 2] if i // 2 < len(nibbles) else 0
        nibble = byte & 0x0F if i % 2 == 0 else byte >> 4
        pred, idx, sample = _decode_nibble(pred, idx, nibble)
        pcm.append(sample)
    return pcm


def _decode_nibble(predictor: int, index: int, nibble: int) -> tuple[int, int, int]:
    code = nibble & 0x0F
    step_index = min(88, max(0, index))
    step = _STEPS[step_index]
    delta = step >> 3
    if code & 4:
        delta += step
    if code & 2:
        delta += step >> 1
    if code & 1:
        delta += step >> 2
    predictor = _clamp(predictor + (-delta if (code & 8) else delta))
    step_index = min(88, max(0, step_index + _ADJUST[code]))
    return predictor, step_index, predictor


def decode_audio_frame(data: bytes) -> list[int] | None:
    if len(data) < 8 or data[0] != FRAME_AUDIO:
        return None
    predictor = int.from_bytes(data[3:5], "little", signed=True)
    index = int.from_bytes(data[5:6], "little", signed=True)
    samples = int.from_bytes(data[6:8], "little")
    return decode(predictor, index, data[8:], samples)


def start_rate(data: bytes) -> int | None:
    if len(data) < 3 or data[0] != FRAME_AUDIO_START:
        return None
    return int.from_bytes(data[1:3], "little")


def transcript_packets(text: str, write_length: int) -> list[bytes]:
    payload = text.encode("utf-8")[:TEXT_LIMIT]
    if not payload:
        return []
    budget = max(20, write_length - 2)
    packets: list[bytes] = []
    offset = 0
    while offset < len(payload):
        count = min(budget, len(payload) - offset)
        flags = 0
        if offset == 0:
            flags |= 0x01
        if offset + count == len(payload):
            flags |= 0x02
        packets.append(bytes((0x10, flags)) + payload[offset:offset + count])
        offset += count
    return packets


def lang_ids(attribute: str) -> set[str]:
    found: set[str] = set()
    for part in attribute.replace(",", ";").split(";"):
        token = part.strip().lower()
        if token.startswith("0x"):
            token = token[2:]
        if not token:
            continue
        try:
            # SAPI reports LANGIDs in hex, so "804" is zh-CN (0x0804), not decimal 804.
            if all(char in "0123456789abcdef" for char in token) and 3 <= len(token) <= 4:
                value = int(token, 16)
            elif token.isdigit():
                value = int(token)
            else:
                continue
        except ValueError:
            continue
        found.add(f"{value:04x}")
    return found


def choose_language(attributes: list[str]) -> str | None:
    have: set[str] = set()
    for attribute in attributes:
        have |= lang_ids(attribute)
    for preferred in LANG_ORDER:
        if preferred in have:
            return preferred
    return None


def language_label(lang_id: str) -> str:
    if lang_id == LANG_CANTONESE:
        return "CANTONESE"
    if lang_id in (LANG_TRADITIONAL, LANG_MANDARIN):
        return "MANDARIN"
    if lang_id == LANG_ENGLISH:
        return "ENGLISH"
    return "LISTENING"


def fallback_language(lang_id: str) -> str:
    if lang_id == LANG_CANTONESE:
        return LANG_ENGLISH
    return LANG_CANTONESE


def self_test() -> bool:
    silent = encode([0, 0, 0, 0])
    if silent != bytes((0, 0)):
        return False
    pcm = [i * 300 for i in range(64)]
    encoded = encode(pcm)
    back = decode(0, 0, encoded, 64)
    max_error = max(abs(pcm[i] - back[i]) for i in range(64))
    if max_error >= 2500:
        return False
    start = bytes((FRAME_AUDIO_START, 0x80, 0x3E))
    if start_rate(start) != 16000:
        return False
    if HELLO != bytes((0x01, 0x50, 0x56, 0x01)):
        return False
    frame = bytes((FRAME_AUDIO, 7, 0, 0, 0, 0, 4, 0)) + silent
    decoded = decode_audio_frame(frame)
    if decoded != [0, 0, 0, 0]:
        return False
    packets = transcript_packets("AB", 23)
    if packets != [bytes((0x10, 0x03, ord("A"), ord("B")))]:
        return False
    wide = transcript_packets("A" * 40, 22)
    if len(wide) != 2 or wide[0][1] != 0x01 or wide[-1][1] != 0x02:
        return False
    if choose_language(["409", "0C04"]) != LANG_CANTONESE:
        return False
    if choose_language(["409"]) != LANG_ENGLISH:
        return False
    if choose_language(["804"]) != LANG_MANDARIN:
        return False
    if language_label(LANG_CANTONESE) != "CANTONESE":
        return False
    if fallback_language(LANG_CANTONESE) != LANG_ENGLISH:
        return False
    if fallback_language(LANG_ENGLISH) != LANG_CANTONESE:
        return False
    return True


def main() -> int:
    if "--self-test" not in sys.argv:
        print("usage: protocol.py --self-test", file=sys.stderr)
        return 2
    ok = self_test()
    print("pc voice protocol self-test: PASS" if ok else "pc voice protocol self-test: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
