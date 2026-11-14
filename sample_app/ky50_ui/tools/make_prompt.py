#!/usr/bin/env python3
"""Offline Mandarin prompt with system eSpeak NG; ffmpeg converts to 16k s16le.
No synthesis engine is shipped on the board. Phrase: 识别成功，请开门。
"""
import ctypes as C
import pathlib
import subprocess
import tempfile
import wave
lib=C.CDLL('libespeak-ng.so.1')
lib.espeak_Initialize.argtypes=[C.c_int,C.c_int,C.c_char_p,C.c_int]
rate=lib.espeak_Initialize(2,0,None,0)
assert rate>0
chunks=[]
CB=C.CFUNCTYPE(C.c_int,C.POINTER(C.c_short),C.c_int,C.c_void_p)
@CB
def collect(p,n,events):
    if p and n: chunks.append(C.string_at(p,n*2))
    return 0
lib.espeak_SetSynthCallback.argtypes=[CB]
lib.espeak_SetSynthCallback(collect)
lib.espeak_SetVoiceByName.argtypes=[C.c_char_p]
assert lib.espeak_SetVoiceByName(b'cmn')==0
lib.espeak_SetParameter(1,155,0)
lib.espeak_Synth.argtypes=[C.c_void_p,C.c_size_t,C.c_uint,C.c_int,C.c_uint,C.c_uint,C.c_void_p,C.c_void_p]
phrase=C.create_string_buffer('识别成功，请开门。'.encode())
assert lib.espeak_Synth(phrase,len(phrase),0,1,0,1,None,None)==0
lib.espeak_Terminate()
out=pathlib.Path(__file__).resolve().parents[1]/'assets/recognition_success.s16le'
with tempfile.TemporaryDirectory() as d:
    wav=pathlib.Path(d)/'prompt.wav'
    with wave.open(str(wav),'wb') as f:
        f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate);f.writeframes(b''.join(chunks))
    subprocess.run(['ffmpeg','-loglevel','error','-y','-i',str(wav),'-ar','16000',
                    '-ac','1','-af','volume=0.7','-f','s16le',str(out)],check=True)
print(out, out.stat().st_size, 'bytes, 16000 Hz mono signed 16-bit little endian')
