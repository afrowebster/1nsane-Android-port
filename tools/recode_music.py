#!/usr/bin/env python3
"""Compress the game's music (16-bit PCM WAV) for the APK: writes "<name>.mp2", which is the original
WAV header (44 bytes, canonical form) followed by MP2 audio. The runtime's mmio layer (win32/extra.c)
serves it to the game as the original WAV, decoding as the game reads.

Usage: recode_music.py in.wav out.mp2
"""
import sys, struct, io
import av

src, dst = sys.argv[1], sys.argv[2]
raw = open(src, 'rb').read()
assert raw[:4] == b'RIFF' and raw[8:12] == b'WAVE', src
pos, fmt, data = 12, None, None
while pos + 8 <= len(raw):
    cid, sz = raw[pos:pos + 4], struct.unpack_from('<I', raw, pos + 4)[0]
    if cid == b'fmt ': fmt = raw[pos + 8:pos + 8 + 16]
    if cid == b'data': data = (pos + 8, min(sz, len(raw) - pos - 8)); break
    pos += 8 + sz + (sz & 1)
tag, ch, rate, _, align, bits = struct.unpack('<HHIIHH', fmt)
assert tag == 1 and bits == 16, 'only 16-bit PCM'
n = data[1] - data[1] % align
hdr = b'RIFF' + struct.pack('<I', 36 + n) + b'WAVEfmt ' + struct.pack('<I', 16) + fmt + b'data' + struct.pack('<I', n)
buf = io.BytesIO()
oc = av.open(buf, 'w', format='mp2')
st = oc.add_stream('mp2', rate=rate)
st.layout = 'stereo' if ch == 2 else 'mono'
st.bit_rate = 224000 if ch == 2 else 128000
pcm = raw[data[0]:data[0] + n]
step = 1152 * 64 * align
pts = 0
for o in range(0, len(pcm), step):
    chunk = pcm[o:o + step]
    fr = av.AudioFrame(format='s16', layout=st.layout.name, samples=len(chunk) // align)
    fr.planes[0].update(chunk)
    fr.sample_rate = rate; fr.pts = pts; pts += len(chunk) // align
    for p in st.encode(fr): oc.mux(p)
for p in st.encode(): oc.mux(p)
oc.close()
open(dst, 'wb').write(hdr + buf.getvalue())
print('%s: %d Hz %d ch %.1f s -> %s (%d KB)' % (src, rate, ch, n / align / rate, dst, (44 + len(buf.getvalue())) // 1024))
