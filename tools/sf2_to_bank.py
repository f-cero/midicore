# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 midicore contributors
#!/usr/bin/env python3
"""
sf2_to_bank.py -- translate SoundFont 2 samples into SYN2 operator programs.

This is the "sample -> program" translator. For every preset it does NOT copy
PCM. It measures the sample and emits a tiny synthesis recipe:

  * a single-cycle wavetable, re-synthesised from the measured harmonic
    amplitudes AND phases of the sample's steady state (one cycle of the
    instrument, 512 bytes, not a half-second of audio)
  * an ADSR fitted to the sample's own amplitude envelope
  * a second operator, only when the sample's attack is measurably brighter
    than its sustain -- that spectral movement is what makes brass bite and
    strings swell, and it is the one thing a static wavetable cannot do

Result: ~24-60 KB for a full GM bank, versus 9.4 MB for the packed samples.

usage: sf2_to_bank.py in.sf2 out.syn [--max-harmonics N] [--quiet]
"""
import struct, sys, math, numpy as np

# --------------------------------------------------------------------- format
MC_TABLE_LEN = 256
MC_PATCH_BYTES = 64
MC_HDR_BYTES = 32
MAGIC = b"SYN2"
VERSION = 1

def crc32(data):
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF

# --------------------------------------------------------------- sf2 parsing
class SF2:
    def __init__(self, path):
        self.raw = open(path, 'rb').read()
        if self.raw[:4] != b'RIFF' or self.raw[8:12] != b'sfbk':
            raise ValueError("not a SoundFont 2 file")
        self.pdta = {}
        self.smpl = b''
        self._walk(12, len(self.raw))

    def _walk(self, off, end):
        while off + 8 <= end:
            cid = self.raw[off:off+4]
            sz = struct.unpack_from('<I', self.raw, off+4)[0]
            body = off + 8
            if cid == b'LIST':
                typ = self.raw[body:body+4]
                if typ == b'pdta':
                    self._walk(body+4, body+sz)
                else:
                    self._walk(body+4, body+sz)
            elif cid == b'smpl':
                self.smpl = self.raw[body:body+sz]
            elif cid in (b'phdr', b'pbag', b'pgen', b'inst', b'ibag', b'igen', b'shdr'):
                self.pdta[cid] = self.raw[body:body+sz]
            off = body + sz + (sz & 1)

    def _recs(self, cid, size):
        d = self.pdta.get(cid, b'')
        n = len(d) // size
        return [d[i*size:(i+1)*size] for i in range(n)]

    def presets(self):
        out = []
        for r in self._recs(b'phdr', 38):
            name = r[:20].split(b'\0')[0].decode('latin1')
            if name == 'EOP': break
            wp, wb, bag = struct.unpack_from('<HHH', r, 20)
            out.append(dict(name=name, bank=wb, prog=wp, bag=bag))
        return out

    def samples(self):
        out = []
        for r in self._recs(b'shdr', 46):
            name = r[:20].split(b'\0')[0].decode('latin1')
            if name == 'EOS': break
            start, end, ls, le, rate = struct.unpack_from('<IIIII', r, 20)
            root = r[40]
            corr = struct.unpack_from('<b', r, 41)[0]
            typ = struct.unpack_from('<H', r, 44)[0]
            out.append(dict(name=name, start=start, end=end, rate=rate,
                            root=root, corr=corr, type=typ))
        return out

    def pcm16(self):
        return np.frombuffer(self.smpl, dtype='<i2')

    def _gens(self, bagrecs, genrecs, idx, nxt):
        """generator list of one zone"""
        out = {}
        for g in range(idx, nxt):
            r = genrecs[g]
            op, val = struct.unpack('<Hh', r)
            if op == 43:   # keyRange
                out['keyLo'] = val & 0xFF
                out['keyHi'] = (val >> 8) & 0xFF
            elif op == 53:
                out['sample'] = val
            elif op == 58:
                out['root'] = val
            elif op == 54:
                out['modes'] = val
            elif op == 34: out['attack'] = val
            elif op == 35: out['decay'] = val
            elif op == 36: out['sustain'] = val
            elif op == 37: out['release'] = val
            elif op == 51: out['coarse'] = val
            elif op == 52: out['fine'] = val
        return out

    def zones_for_preset(self, p):
        pbag = self._recs(b'pbag', 4)
        pgen = self._recs(b'pgen', 4)
        inst = self._recs(b'inst', 22)
        ibag = self._recs(b'ibag', 4)
        igen = self._recs(b'igen', 4)
        shdr = self.samples()

        pbag_ndx = [struct.unpack_from('<H', r, 0)[0] for r in pbag] + [len(pgen)]
        zones = []
        # preset bag -> instrument
        for b in range(p['bag'], p['bag'] + 1):
            gens = self._gens(None, pgen, pbag_ndx[b], pbag_ndx[b+1])
            if 'instrument' in gens: pass
        # find instrument generator in this preset's bags
        inst_idx = None
        for b in range(p['bag'], len(pbag_ndx) - 1):
            g = self._gens(None, pgen, pbag_ndx[b], pbag_ndx[b+1])
            # instrument generator is opcode 41
            for gi in range(pbag_ndx[b], pbag_ndx[b+1]):
                op, val = struct.unpack('<Hh', pgen[gi])
                if op == 41:
                    inst_idx = val
                    break
            if inst_idx is not None:
                break
            # stop when we hit the next preset's first bag
            if b + 1 < len(pbag) and any(struct.unpack('<Hh', pgen[gi])[0] == 41
                                          for gi in range(pbag_ndx[b+1], pbag_ndx[b+2])):
                pass
        if inst_idx is None:
            return zones, shdr
        ibag_ndx = [struct.unpack_from('<H', r, 0)[0] for r in ibag] + [len(igen)]
        for b in range(ibag_ndx[inst_idx], ibag_ndx[inst_idx+1]):
            g = self._gens(None, igen, ibag_ndx[b], ibag_ndx[b+1])
            if 'sample' in g:
                zones.append(g)
        return zones, shdr

# ------------------------------------------------------------------ analysis
def rate_from_seconds(t, lo=0.002, per=9.0):
    """inverse of the engine's rate curve: r = 127 - 9*log2(t/0.002)"""
    if t is None or t <= 0 or not np.isfinite(t):
        return 127
    r = 127.0 - per * math.log2(max(t, lo) / lo)
    return int(max(0, min(127, round(r))))

def fit_envelope(x, rate):
    """Return (attack_s, decay_s, sustain_0_127, release_s) from PCM."""
    if len(x) < 64:
        return 0.01, 0.3, 100, 0.2
    hop = max(1, int(rate * 0.005))
    n = len(x) // hop
    if n < 4:
        return 0.01, 0.3, 100, 0.2
    e = np.sqrt(np.add.reduceat(x[:n*hop].astype(np.float64)**2,
                                np.arange(0, n*hop, hop)) / hop)
    if e.max() <= 0:
        return 0.01, 0.3, 100, 0.2
    e = e / e.max()
    peak = float(e.max())
    if peak <= 0: peak = 1.0

    i_peak = int(np.argmax(e))
    # attack: time to reach 90% of peak
    att = 0.0
    for i in range(0, i_peak + 1):
        if e[i] >= 0.9:
            att = i * 0.005
            break
    if att <= 0:
        att = (i_peak + 1) * 0.005

    # sustain: median of the later, quieter part
    tail = e[int(n*0.55):]
    sus_amp = float(np.median(tail)) if len(tail) else float(e[-1])
    sus_amp = max(0.02, min(1.0, sus_amp))

    # decay: time from peak to within 5% of the sustain level
    dec = 0.0
    target = sus_amp + 0.05 * (peak - sus_amp)
    for i in range(i_peak, n):
        if e[i] <= target:
            dec = (i - i_peak) * 0.005
            break
    if dec <= 0:
        dec = 0.25

    release = 0.15
    if len(tail) >= 2:
        # measure the final fall-off
        k = np.where(e < 0.05)[0]
        if len(k) and k[0] > i_peak:
            release = max(0.02, (n - 1 - k[0]) * 0.005)

    return att, dec, int(max(0, min(127, round(sus_amp * 127)))), release

def harmonic_table(x, rate, root_midi, max_h=96, table_len=MC_TABLE_LEN):
    """Re-synthesise one cycle from the sample's measured harmonics."""
    if len(x) < 2048:
        return None, 0.0
    f0 = 440.0 * 2.0 ** ((root_midi - 69) / 12.0)
    if f0 <= 20 or f0 > rate * 0.4:
        return None, 0.0

    # steady-state window: skip the first 25 %, take up to 16384 frames
    n_fft = 16384
    start = int(len(x) * 0.25)
    seg = x[start:start + n_fft]
    if len(seg) < n_fft:
        seg = np.pad(seg, (0, n_fft - len(seg)))
    win = np.hanning(n_fft)
    spec = np.fft.rfft(seg.astype(np.float64) * win)
    mag = np.abs(spec)
    phs = np.angle(spec)

    bin_hz = rate / n_fft
    amps = np.zeros(max_h + 1)
    phase = np.zeros(max_h + 1)
    for k in range(1, max_h + 1):
        fc = f0 * k
        if fc >= rate * 0.45:
            break
        lo = int(max(0, (fc - bin_hz) / bin_hz))
        hi = int(min(len(mag) - 1, (fc + bin_hz) / bin_hz))
        if hi <= lo:
            continue
        j = lo + int(np.argmax(mag[lo:hi+1]))
        amps[k] = mag[j]
        phase[k] = phs[j]

    if amps.max() <= 0:
        return None, 0.0
    # tilt: keep the spectrum's shape, drop inaudible partials
    amps[amps < amps.max() * 1e-3] = 0

    t = np.arange(table_len) * (2.0 * np.pi / table_len)
    w = np.zeros(table_len)
    for k in range(1, max_h + 1):
        if amps[k] <= 0: continue
        w += amps[k] * np.cos(k * t + phase[k])
    peak = np.abs(w).max()
    if peak <= 1e-9:
        return None, 0.0
    w = w / peak
    # remove any DC the sum left behind
    w -= w.mean()
    w = w / max(np.abs(w).max(), 1e-9)
    return (w * 32000.0).astype(np.int16), float(amps[1:].max())

def brightness_ratio(x, rate, f0):
    """Spectral centroid early vs late. > 1 means the attack is brighter."""
    def centroid(seg):
        if len(seg) < 1024: return 0.0
        n = 4096
        s = seg[:n] if len(seg) >= n else np.pad(seg, (0, n - len(seg)))
        sp = np.abs(np.fft.rfft(s.astype(np.float64) * np.hanning(n)))
        fr = np.fft.rfftfreq(n, 1.0 / rate)
        lo = int(f0 * 0.5 / (rate / n))
        sp[:max(lo, 1)] = 0
        tot = sp.sum()
        return float((fr * sp).sum() / tot) if tot > 0 else 0.0
    early = centroid(x[:int(rate * 0.08)])
    late  = centroid(x[int(len(x) * 0.5):])
    if late <= 0 or early <= 0:
        return 1.0
    return early / late

# ------------------------------------------------------- base wavetable set
def base_tables():
    t = np.arange(MC_TABLE_LEN) * (2 * np.pi / MC_TABLE_LEN)
    tabs = []
    def norm(w):
        w = w - w.mean()
        p = np.abs(w).max()
        return (w / (p if p > 1e-9 else 1) * 32000).astype(np.int16)
    def harm(weights):
        w = np.zeros(MC_TABLE_LEN)
        for k, a in enumerate(weights, start=1):
            w += a * np.sin(k * t)
        return norm(w)

    tabs.append(norm(np.sin(t)))                      # 0 sine
    tabs.append(norm(2/np.pi*np.arcsin(np.sin(t))))   # 1 triangle
    tabs.append(norm(np.linspace(-1, 1, MC_TABLE_LEN, endpoint=False)))  # 2 saw
    tabs.append(harm([1/i for i in range(1, 48) if i % 2 == 1]))         # 3 square
    tabs.append(harm([(1/i) * abs(np.sin(i*np.pi*0.25)) for i in range(1, 48)]))  # 4 25% pulse
    tabs.append(harm([(1/i) * abs(np.sin(i*np.pi*0.125)) for i in range(1, 64)])) # 5 12.5%
    tabs.append(harm([1, 0, 0.33, 0, 0.2, 0, 0.14]))          # 6 organ
    tabs.append(harm([1, 0.5, 0.33, 0.25, 0.2, 0.17, 0.14]))  # 7 soft saw (strings)
    tabs.append(harm([1, 0.72, 0.5, 0.36, 0.26, 0.19, 0.14, 0.1]))  # 8 brass-ish
    tabs.append(harm([1, 0, 0.55, 0, 0.3, 0, 0.18, 0, 0.1]))  # 9 hollow (clarinet)
    tabs.append(harm([1, 0.2, 0.1, 0.05]))                    # 10 mellow
    tabs.append(harm([1, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.15, 0.1]))  # 11 bright
    tabs.append(harm([0, 1, 0]))                              # 12 pure 2nd
    tabs.append(harm([1, 0, 0, 0.5]))                         # 13 organ 4th
    tabs.append(harm([1, 0.6, 0.36, 0.22, 0.13, 0.08]))       # 14 vocal-ish
    tabs.append(harm([1, 0.35, 0.6, 0.2, 0.35, 0.12, 0.2]))   # 15 bell-ish
    while len(tabs) < 16:
        tabs.append(tabs[0])
    return tabs[:16]

# ------------------------------------------------------------------- builder
def build(sf2_path, out_path, max_harm=96, quiet=False):
    sf = SF2(sf2_path)
    pcm = sf.pcm16().astype(np.float64) / 32768.0
    presets = sf.presets()
    tables = base_tables()
    table_index = {}     # signature -> index, so identical waves share a slot
    patches = []

    def add_table(w):
        if w is None:
            return 0
        sig = w.tobytes()[:64]
        if sig in table_index:
            return table_index[sig]
        idx = len(tables)
        tables.append(w)
        table_index[sig] = idx
        return idx

    by_prog = {}
    for p in presets:
        if p['bank'] != 0:
            continue
        if p['prog'] not in by_prog:
            by_prog[p['prog']] = p

    for prog in range(128):
        p = by_prog.get(prog)
        if p is None:
            patches.append(pack_patch(*default_patch(prog, add_table)))
            continue
        try:
            patch = analyse_preset(sf, pcm, p, add_table, max_harm)
        except Exception as ex:
            if not quiet:
                print(f"  [{prog:3d}] {p['name'][:20]:20s} FAILED ({ex}); using default")
            patch = default_patch(prog, add_table)
        patches.append(pack_patch(*patch))
        if not quiet:
            print(f"  [{prog:3d}] {p['name'][:20]:20s} ops={patch[0]} "
                  f"tbl={patch[1]:3d} a={patch[5]:3d} d={patch[6]:3d} s={patch[7]:3d}")

    # drums: percussion presets (bank 128) if present
    drum_src = {}
    for p in presets:
        if p['bank'] == 128 and p['prog'] not in drum_src:
            drum_src[p['prog']] = p
    for note in range(128):
        src = drum_src.get(note) or drum_src.get(note - 35)
        if src is None:
            patches.append(pack_patch(*drum_default(note, add_table)))
            continue
        try:
            patches.append(pack_patch(*analyse_drum(sf, pcm, src, add_table)))
        except Exception:
            patches.append(pack_patch(*drum_default(note, add_table)))

    return write_bank(out_path, tables, patches)

def default_patch(prog, add_table):
    """A tasteful fallback so no GM slot is ever silent."""
    kind = prog // 8
    if kind == 0:    # piano
        tbl, out, a, d, s, r = 10, 120, 118, 108, 0, 96
    elif kind in (1, 2):  # chromatic percussion / organ
        tbl, out, a, d, s, r = 15, 110, 124, 100, 40, 92
    elif kind == 3:  # guitar
        tbl, out, a, d, s, r = 8, 110, 120, 104, 0, 88
    elif kind == 4:  # bass
        tbl, out, a, d, s, r = 2, 118, 126, 84, 60, 96
    elif kind in (5, 6):  # strings / ensemble
        tbl, out, a, d, s, r = 7, 106, 96, 60, 96, 84
    elif kind == 7:  # brass
        tbl, out, a, d, s, r = 8, 112, 104, 70, 88, 86
    elif kind == 8:  # reed
        tbl, out, a, d, s, r = 9, 106, 108, 80, 90, 86
    elif kind == 9:  # pipe
        tbl, out, a, d, s, r = 0, 108, 112, 70, 92, 84
    elif kind == 11: # synth lead
        tbl, out, a, d, s, r = 4, 104, 120, 76, 80, 90
    else:
        tbl, out, a, d, s, r = 10, 106, 110, 80, 70, 88
    return [1, tbl, 1, out, a, d, s, r, 4]

def drum_default(note, add_table):
    tbl = add_table(tables_cache[2]) if False else 2
    return [1, 2, 1, 110, 127, 100, 0, 92, 4]

tables_cache = []

def analyse_preset(sf, pcm, p, add_table, max_harm):
    zones, shdr = sf.zones_for_preset(p)
    if not zones:
        raise ValueError("no instrument zone")
    best = None
    for z in zones:
        si = z.get('sample', 0)
        if si >= len(shdr): continue
        s = shdr[si]
        if s['end'] <= s['start'] + 512: continue
        root = z.get('root', s['root'])
        if root <= 0: root = s['root']
        if root <= 0: root = 60
        length = s['end'] - s['start']
        if best is None or length > best[0]:
            best = (length, s, root, z)
    if best is None:
        raise ValueError("no usable sample")
    _, s, root, z = best
    x = pcm[s['start']:s['end']]
    r = s['rate'] or 44100

    w, _ = harmonic_table(x, r, root, max_h=max_harm)
    if w is None:
        raise ValueError("table synthesis failed")
    tbl = add_table(w)

    att, dec, sus, rel = fit_envelope(x, r)
    br = brightness_ratio(x, r, 440.0 * 2 ** ((root - 69) / 12))
    one_shot = (s['end'] - s['start']) < r * 0.35 or sus < 8

    ra, rd, rr = rate_from_seconds(att), rate_from_seconds(max(dec, 0.02)), rate_from_seconds(rel)

    if br > 1.35 and not one_shot:
        # attack brighter than sustain: add a fast-decaying modulator
        idx = int(min(127, max(8, (br - 1.0) * 55)))
        return [2, tbl, idx, 112, ra, min(127, rd + 14), sus, rr, 4]
    return [1, tbl, 1, min(127, 110 + (10 if one_shot else 0)),
            ra, rd, sus if not one_shot else 0, rr, 4]

def analyse_drum(sf, pcm, p, add_table):
    zones, shdr = sf.zones_for_preset(p)
    if not zones:
        raise ValueError("no zone")
    si = zones[0].get('sample', 0)
    if si >= len(shdr):
        raise ValueError("bad sample")
    s = shdr[si]
    x = pcm[s['start']:s['end']]
    r = s['rate'] or 44100
    w, _ = harmonic_table(x, r, max(30, min(90, s['root'] or 60)), max_h=48)
    if w is None:
        raise ValueError("no table")
    tbl = add_table(w)
    att, dec, sus, rel = fit_envelope(x, r)
    return [1, tbl, 1, 120, rate_from_seconds(att), rate_from_seconds(max(dec, 0.03)),
            0, rate_from_seconds(rel), 2]

# ---------------------------------------------------------------- emit bank
def pack_patch(opcount, tbl, modidx, out, att, dec, sus, rel, algo,
               level=100, gain=104, pan=0, veldepth=96, keytrack=24,
               lfo_mode=0, lfo_rate=64, lfo_delay=0, lfo_vib=0, lfo_trem=0,
               filt=0, filtres=0, filtcut=127, envamt=0, keytrk=0,
               tbl2=0, mul2=2, det2=0):
    """Pack one 64-byte patch. Field order mirrors mc_patch in mc_bank.h exactly;
    any mismatch here silently corrupts every field after the first slip, so the
    layout is expressed once as a struct format instead of byte indices."""
    # 20 bytes of patch fields, then 4 reserved, then 4 x 10-byte operators
    hdr = struct.pack('<16BbbBBxxxx',
        max(1, min(4, opcount)) & 0xFF,   # 0  opCount
        algo & 3,                          # 1  algorithm
        0,                                 # 2  flags
        veldepth & 127,                    # 3  velDepth
        keytrack & 127,                    # 4  keyTrack
        filt & 3,                          # 5  filterMode
        filtres & 127,                     # 6  filterRes
        filtcut & 127,                     # 7  filterCut
        envamt & 127,                      # 8  filterEnvAmt
        keytrk & 127,                      # 9  filterKeyTrk
        lfo_mode & 3,                      # 10 lfoMode
        lfo_rate & 127,                    # 11 lfoRate
        lfo_delay & 255,                   # 12 lfoDelay
        lfo_vib & 127,                     # 13 lfoVib
        lfo_trem & 127,                    # 14 lfoTrem
        gain & 127,                        # 15 gain
        pan,                               # 16 pan
        0,                                 # 17 pitchEnvAmt
        0,                                 # 18 pitchEnvRate
        0)                                 # 19 pad0

    def op(table, mul, lvl, att_, dec_, sus_, rel_, out_, det=0, ks=24):
        return struct.pack('<BBBBbBBBBB',
            table & 0xFF, mul & 0xFF, lvl & 127, ks & 127,
            det, att_ & 127, dec_ & 127, sus_ & 127, rel_ & 127, out_ & 127)

    ops = op(tbl, 1, level, att, dec, sus, rel, out)
    ops += op(tbl2, mul2, modidx, att, min(127, dec + 18), 0, rel, 0, det2)
    ops += op(0, 1, 0, att, dec, 0, rel, 0)
    ops += op(0, 1, 0, att, dec, 0, rel, 0)
    b = hdr + ops
    assert len(b) == MC_PATCH_BYTES, f"patch is {len(b)} bytes, expected {MC_PATCH_BYTES}"
    return b

def write_bank(path, tables, patches):
    tbl_bytes = b''.join(np.asarray(t, dtype='<i2').tobytes() for t in tables)
    pat_bytes = b''.join(patches)
    tbl_off = MC_HDR_BYTES
    pat_off = tbl_off + len(tbl_bytes)
    total = pat_off + len(pat_bytes)
    body = tbl_bytes + pat_bytes
    hdr = struct.pack('<4sHHHHIIIII',
                      MAGIC, VERSION, MC_HDR_BYTES,
                      len(tables), len(patches),
                      tbl_off, pat_off, total, crc32(body), 0)
    assert len(hdr) == MC_HDR_BYTES, len(hdr)
    with open(path, 'wb') as f:
        f.write(hdr + body)
    return len(tables), len(patches), total

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    max_h = 96
    if '--max-harmonics' in sys.argv:
        max_h = int(sys.argv[sys.argv.index('--max-harmonics') + 1])
    quiet = '--quiet' in sys.argv
    nt, np_, total = build(src, dst, max_h, quiet)
    print(f"bank: {nt} tables, {np_} patches, {total} bytes ({total/1024:.1f} KB)")
    return 0

if __name__ == '__main__':
    sys.exit(main())
