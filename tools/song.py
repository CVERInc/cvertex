#!/usr/bin/env python3
"""song — bake a tracker table into C.

Same bargain as obj2mesh and svg2poly: the thinking happens here, at build time, and the
engine only looks things up. A song is patterns plus an arrangement; what ships is neither,
it's the flattened table, because the flattened table is the only thing playback needs.

    tools/song.py > games/dash_song.h

Rows are sixteenth notes. Five channels — the sixth belongs to sound effects and is never
read. Nothing here knows what cvertex is; it prints numbers.
"""
import sys

NCHAN = 6
RPS = 10                      # rows per second: sixteenths at 150 BPM
BAR = 16                      # sixteenths in a bar of 4/4

# Scale degrees in A natural minor, as MIDI. Mega Man lives in minor and so does this.
A2, C3, D3, E3, F3, G3 = 45, 48, 50, 52, 53, 55
A3, B3, C4, D4, E4, F4, G4 = 57, 59, 60, 62, 64, 65, 67
A4, B4, C5, D5, E5, F5, G5, A5 = 69, 71, 72, 74, 76, 77, 79, 81

KICK, SNARE, HAT = 36, 50, 74

# chord -> (bass root, arpeggio cycle). The arp is the engine of the genre: three notes at a
# speed no hand plays, which is how two pulse channels ever sounded like a band.
CHORDS = {
    'Am': (A2, [A3, C4, E4, C4]),
    'F':  (F3, [F3, A3, C4, A3]),
    'C':  (C3, [C4, E4, G4, E4]),
    'G':  (G3, [G3, B3, D4, B3]),
    'Dm': (D3, [D4, F4, A4, F4]),
    'E':  (E3, [E4, G4, B4, G4]),
}

def bass(chord, drive):
    """Eighths on the root, with an octave kick on the offbeat when it's driving."""
    root = CHORDS[chord][0]
    row = [0] * BAR
    for i in range(0, BAR, 2):
        row[i] = root
    if drive:
        row[6] = root + 12
        row[14] = root + 12
    return row

def arp(chord):
    cyc = CHORDS[chord][1]
    return [cyc[i % len(cyc)] for i in range(BAR)]

def drums(fill):
    row = [0] * BAR
    for i in range(0, BAR, 2):
        row[i] = HAT
    row[0] = KICK
    row[4] = SNARE
    row[8] = KICK
    row[10] = KICK
    row[12] = SNARE
    if fill:
        for i in (13, 14, 15):
            row[i] = SNARE
    return row

def lead(notes):
    """notes: list of (row, midi). Everything else holds."""
    row = [0] * BAR
    for r, n in notes:
        row[r] = n
    return row

# --- the tune ------------------------------------------------------------------
# A: the hook. B: the same harmony climbing. C: the bridge that refuses to land.
# It never resolves — Am at the end goes straight back to Am at the start, so the loop has
# no seam and no arrival. A tune that arrived would keep telling you you'd got somewhere.
PROG_A = ['Am', 'F', 'C', 'G']
PROG_B = ['Am', 'F', 'G', 'E']
PROG_C = ['Dm', 'Dm', 'E', 'E']

HOOK = [
    [(0, A4), (3, C5), (6, E5), (8, D5), (12, C5), (14, B4)],
    [(0, C5), (3, A4), (6, F4), (8, A4), (12, C5), (14, E5)],
    [(0, G4), (3, C5), (6, E5), (8, G5), (12, E5), (14, C5)],
    [(0, D5), (3, B4), (6, G4), (8, B4), (12, D5), (14, F5)],
]
CLIMB = [
    [(0, E5), (2, A4), (4, E5), (6, A4), (8, C5), (12, E5)],
    [(0, F5), (2, C5), (4, F5), (6, C5), (8, A4), (12, F4)],
    [(0, G5), (2, D5), (4, G5), (6, D5), (8, B4), (12, G4)],
    [(0, A5), (4, G5), (8, E5), (11, D5), (14, B4)],
]
BRIDGE = [
    [(0, D5), (4, F5), (8, A5), (12, F5)],
    [(0, E5), (4, D5), (8, C5), (12, A4)],
    [(0, B4), (4, E5), (8, G5), (12, E5)],
    [(0, G5), (2, F5), (4, E5), (6, D5), (8, C5), (10, B4), (12, A4), (14, G4)],
]

def section(prog, melodies, drive=True, fill_last=True, bassless=False):
    rows = []
    for b, chord in enumerate(prog):
        ld, ar, dr = lead(melodies[b]), arp(chord), drums(fill_last and b == len(prog) - 1)
        bs = [0] * BAR if bassless else bass(chord, drive)
        # ch0 lead, ch1 harmony (the arp an octave down, which is the second pulse),
        # ch2 bass, ch3 arp, ch4 drums, ch5 never.
        for i in range(BAR):
            rows.append([ld[i], (ar[i] - 12) if i % 4 == 0 else 0, bs[i], ar[i], dr[i], 0])
    return rows

# One tune per act. The song used to be 25.6 seconds against acts of 33, so it drifted:
# every loop landed somewhere different in the street and the two never agreed about
# anything. An act that has its own tune STARTS with it, and starting together is the only
# way a beat ever tells you where you are.
#
# Same skeleton, different clothes, because they're three views of one run and not three
# games. The street is the hook. Indoors drops the drums to a pulse and lets the arp carry
# it, the way a corridor sounds. The roof takes the bass away entirely for four bars — the
# only quiet in the game, and it lands where you're highest.
def build(name, plan):
    rows = []
    for prog, mel, kw in plan:
        rows += section(prog, mel, **kw)
    return name, rows

TUNES = [
    build("STREET", [(PROG_A, HOOK, {}), (PROG_B, CLIMB, {}),
                     (PROG_A, HOOK, {}), (PROG_C, BRIDGE, {})]),
    build("INSIDE", [(PROG_B, CLIMB, {"drive": False}), (PROG_A, HOOK, {"drive": False}),
                     (PROG_C, BRIDGE, {}), (PROG_B, CLIMB, {})]),
    build("ROOF",   [(PROG_C, BRIDGE, {"bassless": True, "fill_last": False}),
                     (PROG_A, HOOK, {}), (PROG_B, CLIMB, {}), (PROG_A, HOOK, {})]),
]
ROWS = len(TUNES[0][1])
assert all(len(r) == ROWS for _, r in TUNES)

print("// Generated by tools/song.py — do not edit. Regenerate instead.")
print("//")
print("// %d tunes, %d rows each at %d/sec = %.1f seconds round, five channels, %d bytes total."
      % (len(TUNES), ROWS, RPS, ROWS / RPS, len(TUNES) * ROWS * NCHAN))
print("#define SONG_ROWS %d" % ROWS)
print("#define SONG_RPS  %d" % RPS)
print("#define SONG_N    %d" % len(TUNES))
print("static const uint8_t SONG[SONG_N][SONG_ROWS][NCHAN] = {")
for name, rows in TUNES:
    print("  {  // %s" % name)
    for i in range(0, ROWS, 2):
        a = ",".join("%3d" % v for v in rows[i])
        b = ",".join("%3d" % v for v in rows[i + 1])
        print("    {%s}, {%s}," % (a, b))
    print("  },")
print("};")
print("// lead / harmony / bass / arp / drums / (sfx)")
print("static const uint8_t SONG_INSTR[NCHAN] = { 0, 0, 2, 5, 3, 0 };")
