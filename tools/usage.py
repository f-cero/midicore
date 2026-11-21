# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 midicore contributors
#!/usr/bin/env python3
"""usage.py -- which GM programs a MIDI file actually plays, and for how long."""
import sys, struct, collections

def parse(path):
    b = open(path,'rb').read()
    hl = int.from_bytes(b[4:8],'big'); ntr = int.from_bytes(b[10:12],'big')
    div = int.from_bytes(b[12:14],'big')
    pos = 8+hl; events=[]
    for _ in range(ntr):
        ln = int.from_bytes(b[pos+4:pos+8],'big'); p=pos+8; end=p+ln; pos=end
        tick=0; status=0
        while p<end:
            d=0
            while True:
                c=b[p]; p+=1; d=(d<<7)|(c&0x7f)
                if not c&0x80: break
            tick+=d; c=b[p]
            if c&0x80: status=c; p+=1
            hi=status&0xf0
            if status==0xff:
                mt=b[p]; p+=1; l=0
                while True:
                    cc=b[p]; p+=1; l=(l<<7)|(cc&0x7f)
                    if not cc&0x80: break
                p+=l
            elif status in (0xf0,0xf7):
                l=0
                while True:
                    cc=b[p]; p+=1; l=(l<<7)|(cc&0x7f)
                    if not cc&0x80: break
                p+=l
            else:
                na=1 if hi in (0xc0,0xd0) else 2
                a=b[p:p+na]; p+=na
                events.append((tick,status&0x0f,hi,a))
    return div, events

GM = ['Acoustic Grand Piano','Bright Acoustic Piano','Electric Grand Piano','Honky-tonk Piano',
'EPiano 1 (Rhodes)','EPiano 2 (Chorused)','Harpsichord','Clavinet','Celesta','Glockenspiel',
'Music Box','Vibraphone','Marimba','Xylophone','Tubular Bells','Dulcimer','Drawbar Organ',
'Percussive Organ','Rock Organ','Church Organ','Reed Organ','Accordion','Harmonica',
'Tango Accordion','Nylon Guitar','Steel Guitar','Jazz Guitar','Clean Guitar','Muted Guitar',
'Overdriven Guitar','Distortion Guitar','Guitar Harmonics','Acoustic Bass','Fingered Bass',
'Picked Bass','Fretless Bass','Slap Bass 1','Slap Bass 2','Synth Bass 1','Synth Bass 2',
'Violin','Viola','Cello','Contrabass','Tremolo Strings','Pizzicato Strings','Harp','Timpani',
'String Ensemble 1','String Ensemble 2','Synth Strings 1','Synth Strings 2','Choir Aahs',
'Voice Oohs','Synth Voice','Orchestra Hit','Trumpet','Trombone','Tuba','Muted Trumpet',
'French Horn','Brass Section','Synth Brass 1','Synth Brass 2','Soprano Sax','Alto Sax',
'Tenor Sax','Baritone Sax','Oboe','English Horn','Bassoon','Clarinet','Piccolo','Flute',
'Recorder','Pan Flute','Blown Bottle','Shakuhachi','Whistle','Ocarina','Square Lead',
'Saw Lead','Calliope Lead','Chiff Lead','Charang Lead','Voice Lead','Fifths Lead',
'Bass+Lead','New Age Pad','Warm Pad','Polysynth Pad','Choir Pad','Bowed Pad','Metallic Pad',
'Halo Pad','Sweep Pad','Rain','Soundtrack','Crystal','Atmosphere','Brightness','Goblins',
'Echoes','Sci-Fi','Sitar','Banjo','Shamisen','Koto','Kalimba','Bagpipe','Fiddle','Shanai',
'Tinkle Bell','Agogo','Steel Drums','Woodblock','Taiko Drum','Melodic Tom','Synth Drum',
'Reverse Cymbal','Guitar Fret Noise','Breath Noise','Seashore','Bird Tweet','Telephone',
'Helicopter','Applause','Gunshot']

for path in sys.argv[1:]:
    div, ev = parse(path)
    prog = collections.defaultdict(int)
    used = collections.Counter(); chans = collections.defaultdict(set)
    for tick,ch,hi,a in ev:
        if hi==0xc0: prog[ch]=a[0]
        elif hi==0x90 and a[1]>0:
            used[(prog[ch], prog[ch] if ch!=9 else 'drum')] += 1
            chans[prog[ch]].add(ch)
    print(f"\n{path.split('/')[-1]}  ({len(ev)} events)")
    tot = sum(used.values())
    for (p, _), n in used.most_common():
        if p == 'drum':
            print(f"   ch9  drums         {n:5d} notes")
            continue
        print(f"   ch{sorted(chans[p])} prog {p:3d} {GM[p]:22s} {n:5d} notes  {100*n/tot:4.1f}%")
