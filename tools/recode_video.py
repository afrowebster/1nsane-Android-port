#!/usr/bin/env python3
"""Re-encode the game's Indeo 5 AVI videos as MPEG-1 video + MP2 audio in an MPEG program stream,
which the runtime decodes with pl_mpeg (win32/video.c). Nothing else on the device can decode Indeo.

Usage: recode_video.py in.avi out.mpg
"""
import sys
import av

src, dst = sys.argv[1], sys.argv[2]
ic = av.open(src)
iv = ic.streams.video[0]
ia = ic.streams.audio[0] if ic.streams.audio else None
oc = av.open(dst, 'w', format='mpeg')
ov = oc.add_stream('mpeg1video', rate=25)
ov.width, ov.height = iv.codec_context.width, iv.codec_context.height
ov.pix_fmt = 'yuv420p'
ov.bit_rate = 3_000_000
ov.codec_context.options = {'qmax': '10', 'g': '25', 'bf': '0'}   # pl_mpeg handles B frames, but none keeps decoding cheap
oa = None
if ia is not None:
    oa = oc.add_stream('mp2', rate=44100)
    oa.layout = 'stereo'
    oa.bit_rate = 224_000
    resampler = av.AudioResampler(format='s16', layout='stereo', rate=44100)
nv = 0
for pkt in ic.demux():
    for fr in pkt.decode():
        if isinstance(fr, av.VideoFrame):
            # place frames by their timestamp: Indeo "drop" frames are missing and must repeat the previous one
            want = int(round(float(fr.pts * fr.time_base) * 25)) if fr.pts is not None else nv
            f2 = fr.reformat(format='yuv420p')
            while nv <= want:
                g = av.VideoFrame.from_ndarray(f2.to_ndarray(), format='yuv420p') if nv < want else f2
                g.pts = nv; nv += 1
                for p in ov.encode(g): oc.mux(p)
        elif oa is not None:
            fr.pts = None
            for r in resampler.resample(fr):
                for p in oa.encode(r): oc.mux(p)
for p in ov.encode(): oc.mux(p)
if oa is not None:
    for p in oa.encode(): oc.mux(p)
oc.close()
import os
print('%s: %dx%d, %d frames -> %s (%d KB)' % (src, ov.width, ov.height, nv, dst, os.path.getsize(dst) // 1024))
