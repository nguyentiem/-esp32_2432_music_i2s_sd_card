#!/usr/bin/env python3
"""Generate a quiet 8-second PCM WAV and matching Vietnamese lyrics; no dependencies."""
import math
import struct
import wave
from pathlib import Path
root = Path(__file__).resolve().parents[1] / 'sdcard' / 'music'
root.mkdir(parents=True, exist_ok=True)
rate = 22050
with wave.open(str(root / 'demo.wav'), 'wb') as out:
    out.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
    for second in range(8):
        frequency = [262, 294, 330, 349, 392, 440, 494, 523][second]
        samples = []
        for i in range(rate):
            envelope = min(1, i / 220, (rate - i) / 220)
            samples.append(struct.pack('<h', round(3500 * envelope * math.sin(2 * math.pi * frequency * i / rate))))
        out.writeframes(b''.join(samples))
(root / 'demo.lrc').write_text('[00:00.00]Xin chào ESP32!\n[00:02.00]Nhạc và lời cùng nhịp\n[00:04.00]Màn hình tiếng Việt\n[00:06.00]Chúc bạn nghe nhạc vui vẻ!\n', encoding='utf-8')
print(root / 'demo.wav')
