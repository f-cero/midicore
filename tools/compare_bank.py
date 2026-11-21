# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 midicore contributors
#!/usr/bin/env python3
"""
compare_bank.py -- measure each synthesized program against its source sample.

For every GM program: take the sample the converter chose, and compare its
harmonic structure (pitch-independent) with what the engine actually synthesises
for that program. Flags what is worth looking at.

usage: compare_bank.py sf2 bank.syn [outdir]
"""
import os, sys, struct, subprocess, json, math
import numpy as np
import importlib.util

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('sf2mod', os.path.join(ROOT, 'tools', 'sf2_to_bank.py'))
sf2m = importlib.util.module_from_spec(spec); spec.loader.exec_module(sf2m)

RATE = 32000
GM = ['Acoustic Grand','Bright Piano','Elec Grand','Honky-tonk','EPiano 1','EPiano 2',
'Harpsichord','Clavinet','Celesta','Glockenspiel','Music Box','Vibraphone','Marimba',
'Xylophone','Tubular Bells','Dulcimer','Drawbar Organ','Percussive Organ','Rock Organ',
'Church Organ','Reed Organ','Accordion','Harmonica','Tango Accordion','Nylon Guitar',
'Steel Guitar','Jazz Guitar','Clean Guitar','Muted Guitar','Overdriven Gtr','Distortion Gtr',
'Gtr Harmonics','Acoustic Bass','Fingered Bass','Picked Bass','Fretless Bass','Slap Bass 1',
'Slap Bass 2','Synth Bass 1','Synth Bass 2','Violin','Viola','Cello','Contrabass',
'Tremolo Strings','Pizzicato Str','Harp','Timpani','Strings 1','Strings 2','Synth Str 1',
'Synth Str 2','Choir Aahs','Voice Oohs','Synth Voice','Orchestra Hit','Trumpet','Trombone',
'Tuba','Muted Trumpet','French Horn','Brass Section','Synth Brass 1','Synth Brass 2',
'Soprano Sax','Alto Sax','Tenor Sax','Bari Sax','Oboe','English Horn','Bassoon','Clarinet',
'Piccolo','Flute','Recorder','Pan Flute','Blown Bottle','Shakuhachi','Whistle','Ocarina',
'Square Lead','Saw Lead','Calliope Lead','Chiff Lead','Charang Lead','Voice Lead',
'Fifths Lead','Bass+Lead','New Age Pad','Warm Pad','Polysynth Pad','Choir Pad','Bowed Pad',
'Metallic Pad','Halo Pad','Sweep Pad','Rain','Soundtrack','Crystal','Atmosphere','Brightness',
'Goblins','Echoes','Sci-Fi','Sitar','Banjo','Shamisen','Koto','Kalimba','Bagpipe','Fiddle',
'Shanai','Tinkle Bell','Agogo','Steel Drums','Woodblock','Taiko','Melodic Tom','Synth Drum',
'Reverse Cymbal','Fret Noise','Breath','Seashore','Bird','Telephone','Helicopter','Applause','Gunshot']

def harmonics(x, f0, rate, nh=24):
    n = min(len(x), 16384)
    if n < 1024: return None
    seg = x[:n].astype(np.float64)
    seg = seg - seg.mean()
    sp = np.abs(np.fft.rfft(seg * np.hanning(n)))
    bin_hz = rate / n
    out = np.zeros(nh)
    for k in range(1, nh + 1):
        fc = f0 * k
        if fc >= rate * 0.45: break
        lo = max(1, int(fc / bin_hz) - 2); hi = int(fc / bin_hz) + 2
        if hi >= len(sp): break
        out[k-1] = sp[lo:hi+1].max()
    if out.max() <= 0: return None
    return out / out.max()

def centroid_k(x, f0, rate):
    n = min(len(x), 16384)
    if n < 1024: return 0.0
    sp = np.abs(np.fft.rfft((x[:n] - x[:n].mean()) * np.hanning(n)))
    fr = np.fft.rfftfreq(n, 1.0 / rate)
    lo = int(f0 * 0.5 / (rate / n))
    sp[:max(lo, 1)] = 0
    tot = sp.sum()
    return float((fr * sp).sum() / tot / f0) if tot > 0 else 0.0

def build_midi(path):
    DIV = 480
    def vlq(n):
        o = [n & 0x7F]; n >>= 7
        while n: o.append((n & 0x7F) | 0x80); n >>= 7
        return bytes(reversed(o))
    ev = [(0, b'\xff\x51\x03\x07\xa1\x20')]   # 500000 us/quarter
    t = 0
    for p in range(128):
        ev.append((t, bytes([0xC0, p])))
        ev += [(t, bytes([0x90, 60, 100])), (t + DIV * 3, bytes([0x80, 60, 0]))]
        t += DIV * 4
    ev.sort(key=lambda e: e[0])
    body = bytearray(); last = 0
    for tick, data in ev:
        body += vlq(tick - last) + data; last = tick
    body += vlq(0) + b'\xff\x2f\x00'
    open(path, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, DIV) +
                           b'MTrk' + struct.pack('>I', len(body)) + bytes(body))

def main():
    sf2_path, bank_path = sys.argv[1], sys.argv[2]
    outdir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, 'build', 'compare')
    os.makedirs(outdir, exist_ok=True)

    sf = sf2m.SF2(sf2_path)
    pcm = sf.pcm16().astype(np.float64) / 32768.0
    presets = sf.presets()
    shdr = sf.samples()
    by_prog = {}
    for p in presets:
        if p['bank'] == 0 and p['prog'] not in by_prog:
            by_prog[p['prog']] = p

    mid = os.path.join(outdir, 'sweep.mid')
    build_midi(mid)
    wav = os.path.join(outdir, 'sweep.wav')
    subprocess.run([os.path.join(ROOT, 'build', 'hr'), bank_path, mid, wav,
                    str(128 * 2.0), str(RATE), '8'], check=True,
                   capture_output=True)

    import wave
    w = wave.open(wav)
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
    w.close()
    x = x.reshape(-1, 2).mean(1)

    rows = []
    for p in range(128):
        t0 = p * 2.0 + 0.35
        seg = x[int(t0*RATE):int((t0+1.2)*RATE)]
        f0s = 440.0 * 2 ** ((60 - 69) / 12)
        h_syn = harmonics(seg, f0s, RATE)
        c_syn = centroid_k(seg, f0s, RATE)
        lvl = float(np.sqrt(np.mean(seg**2))) if len(seg) else 0.0

        info = dict(prog=p, name=GM[p], root=None, cos=None, cent_syn=c_syn,
                    cent_smp=None, lvl_db=20*math.log10(lvl+1e-9), label='')
        pr = by_prog.get(p)
        if pr is not None:
            try:
                zones, sh = sf.zones_for_preset(pr)
                best = None
                for z in zones:
                    si = z.get('sample', 0)
                    if si >= len(sh): continue
                    s = sh[si]
                    if s['end'] <= s['start'] + 512: continue
                    L = s['end'] - s['start']
                    if best is None or L > best[0]: best = (L, s, z)
                if best:
                    _, s, z = best
                    root = z.get('root', s['root']) or s['root'] or 60
                    info['root'] = root
                    smp = pcm[s['start']:s['end']]
                    n0 = int(len(smp) * 0.25)
                    smp = smp[n0:]
                    f0m = 440.0 * 2 ** ((root - 69) / 12)
                    h_smp = harmonics(smp, f0m, s['rate'] or 44100)
                    info['cent_smp'] = centroid_k(smp, f0m, s['rate'] or 44100)
                    if h_syn is not None and h_smp is not None:
                        a = h_syn / (np.linalg.norm(h_syn) + 1e-9)
                        b = h_smp / (np.linalg.norm(h_smp) + 1e-9)
                        info['cos'] = float(a @ b)
                    info['smp_h'] = [round(float(v), 3) for v in (h_smp if h_smp is not None else [])]
                    info['smp_len_ms'] = round(1000.0 * len(smp) / (s['rate'] or 44100))
            except Exception as ex:
                info['label'] = f'zone-error:{type(ex).__name__}'
        info['syn_h'] = [round(float(v), 3) for v in (h_syn if h_syn is not None else [])]
        rows.append(info)

    print(f"{'prog':>4} {'name':<16} {'root':>4} {'cos':>5} {'centK syn/smp':>14} {'lvl dB':>7}  flags")
    flags_all = []
    for r in rows:
        fl = []
        if r['cos'] is not None and r['cos'] < 0.55: fl.append('timbre')
        if r['root'] is not None and (r['root'] < 30 or r['root'] > 84): fl.append('root!')
        if r['cent_smp'] and r['cent_syn']:
            ratio = r['cent_syn'] / max(r['cent_smp'], 1e-6)
            if ratio > 2.2: fl.append('too-bright')
            elif ratio < 0.45: fl.append('too-dull')
        if r['lvl_db'] < -42: fl.append('quiet')
        if r['lvl_db'] > -12: fl.append('LOUD')
        if r['label']: fl.append(r['label'])
        cos_s = ('%.2f' % r['cos']) if r['cos'] is not None else '  - '
        smp_s = ('%.2f' % r['cent_smp']) if r['cent_smp'] else '  - '
        print('%4d %-16s %4s %5s %6.2f/%-6s %7.1f  %s' % (
            r['prog'], r['name'], str(r['root']), cos_s,
            r['cent_syn'], smp_s, r['lvl_db'], ','.join(fl)))
        if fl: flags_all.append((r['prog'], r['name'], fl))

    json.dump(rows, open(os.path.join(outdir, 'compare.json'), 'w'), indent=1)
    print(f"\n{len(flags_all)} programs flagged")
    return 0

if __name__ == '__main__':
    sys.exit(main())
