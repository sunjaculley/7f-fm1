# 7F

An adaptation of [Fors FMS](https://lo-bit.club/fms/guide), the Game Boy Advance FM groovebox, for the M-VAVE FM-1.
Four 2-op FM tracks and a noise track, sixteen steps that are each their own sound, note echo, a transpose
sequence, step modulators, scales, a palette, a snapshot and pattern slots, mapped to the FM-1's keys and knobs.
Unofficial: FMS is by Fors, and this firmware uses none of its code. Made with [foma](https://github.com/konsumer/foma).

**Try it in the browser, and install it on the FM-1 from Chrome or Edge:** <https://sunjaculley.github.io/7f-fm1/>

## The idea, as in FMS

- **Every step is a sound.** No instruments or presets: a step holds its note, level, pan, portamento, condition,
  wait, chord, and its whole FM patch. A new step takes the sound of the last step you touched, like a pen.
- **2-op FM.** A modulator, feeding back into itself, into a carrier. MULT is the modulator's ratio: a whole number
  0-F plus a fraction (1/8, 1/4, 1/3, 1/2, 2/3, 3/4, .975). MODE `+` mixes the two operators instead (2-note chords,
  detune, additive), both through the mod envelope. Amp envelope (attack, hold, release), mod envelope (attack,
  release, end level), pitch sweep (depth, release).
- **Noise.** GB (15-bit LFSR), WH (31-bit), ML (metallic, six squares), GR (grainy), RS (resonant bandpass). RATE
  narrows each one: LFSR length, partials, density, clock. FILT is a highpass (on RS, the resonance).
- **Chords.** Up to three offsets a step. Each takes another FM voice if one is free or fading, top-down by track,
  so material on track 4 is the last to be taken. A track's own trig always takes its voice back.
- Values read in hex, 00-7F.

## The panel

The screen: the five tracks and their sixteen steps at the top (cursor in white, playheads bright, trigless steps
hollow, small marks for chord, echo and condition), then one setting drawn across all sixteen steps of the track,
then a picture of the page, then the four knobs. KNOB1-4 are cyan, lime, white and orange, and whatever a knob
moves on screen wears its colour.

| Control | What it does |
| --- | --- |
| White keys | The steps of the selected track. Tap: on or off. Double tap: trigless (changes the sound without restarting it, for slides and stepped automation). Hold a step and turn a knob: edit that step without toggling it. Hold two steps: everything between them is edited together |
| SELECT | Move the cursor along the steps (on TRANSPOSE: along the transpose stops) |
| KNOB1-4 | The page's four settings, for the cursor step (or the held steps). Fast turns move further |
| Hold SEQ + knob | Every step of the track at once (each moves by the same amount) |
| Hold REC + knob | Every step of every track. With TRACK settings (length, rate, direction, shuffle): every track |
| F#3 G#3 A#3 C#4 D#4 | Tracks 1, 2, 3, 4, N: select and hear the cursor step |
| F#4 · G#4 | COPY · PASTE: held steps, else the whole track |
| A#4 | COOK: an incremental random edit (held steps, the track, or with REC every track). Press again for more |
| C#5 | CLEAR: held steps, else the track, back to the clean slate |
| Hold D#5 + track key | Mute. Tap D#5 alone: everything back on |
| Hold F#5 + track key | Solo |
| OCT- / OCT+ | Previous / next track. With SEQ held: rotate the track (with REC too: every track) |
| ALGORITHM | Tempo |
| PRESETS | Browse the palette into the cursor step (or held steps; SEQ: the track): X, ?, then the stored sounds |
| PLAY | Start / stop |
| SAVE | Tap: swap between the snapshot and the live state. Hold 0.8 s: take a snapshot (the header flashes) |
| Hold GLO | White key 1 tap tempo (four taps), 2 / 3 tempo -10 / +10 while held, 4 restart |
| HOME | PATTERNS (again: back) |
| ARP | PALETTE (again: back) |

**Pages.** Press a page button again for its next page. Pages that only FM has are skipped on the noise track.

| Button | Page | KNOB1 | KNOB2 | KNOB3 | KNOB4 |
| --- | --- | --- | --- | --- | --- |
| EDIT | NOTE | NOTE (in the scale) | LEVEL | PAN L/C/R | PORT |
| | TRIG | COND (1:n, every nth pass) | WAIT (sixths of a step) | ECHO on/off | TSP on/off |
| | CHORD | CHORD 1 | CHORD 2 | CHORD 3 | MODE FM / + |
| SEL | FM | MULT | FRAC | DEPTH | FDBK |
| | NOISE | TYPE | RATE | FILT | |
| ENV | AMP | ATTACK | HOLD | RELEASE | LEVEL |
| | MOD ENV | M.ATK | M.REL | M.END | DEPTH |
| | SWEEP | SWEEP | SW.REL | PORT | NOTE |
| LFO | MODULATOR | DEST | SPEED (steps) | WAVE | DEPTH |
| | MOD 2 | TRACK (target) | OFFSET | | |
| FX | ECHO | REP | SPD (sub-step . steps) | VOL a repeat | PAN C/L/R/LR |
| | ECHO 2 | MOD a repeat | FBK a repeat | TSP a repeat | MODULO |
| SEQ | TRACK | LENGTH | RATE (6x-1/4) | DIRECTION | SHUFFLE |
| GLO | GLOBAL | TEMPO | SCALE | KEY | SYNC (INT, OUT, IN) |
| | TRANSPOSE | LENGTH (1-8) | RATE (repeats) | ADV (PATTERN, STEP, TRIG) | VALUE |
| | SYSTEM | DAC (CLEAN, GBA) | THEME (DARK, LIGHT) | | |

**Note echo** (per pattern) repeats steps whose ECHO is on: REP times, SPD apart, each repeat changing level, FM
depth and feedback by the set amounts, panned as set (LR ping-pongs), transposed by TSP a repeat and wrapped by
MODULO, so a repeat can arpeggiate. Repeats retrigger the track's own voice.

**Modulator** (one per pattern): a sample-and-hold LFO that moves one step setting of a target track, one value per
trig of that track. SPEED is its length in steps, WAVE ramp down, ramp up, triangle, square or random.

**Transpose sequence** (global): up to 8 stops, in semitones and then into the scale, on the FM steps whose TSP is
on. It moves on every bar (PATTERN), every step (STEP) or every trig of track 1 (TRIG), after RATE repeats.

**Scale**: 16 of them and a key. It filters note entry and quantizes what plays, without changing what is stored.

**PATTERNS** (HOME). Five slots per track, besides the live pattern. White keys 1-5 cue a slot for the selected
track: it starts when the track's pattern ends (when stopped: at once). SEQ + key loads it now, keeping the
playhead; SEL + key loads it and restarts the track on the next beat. SAVE + key stores the live pattern (saving an
empty track clears the slot). Add REC for the whole column. 6 clears the live track, 7 makes a random one, 16 locks
the slots.

**PALETTE** (ARP). 12 sounds for FM and 12 for noise, shared by every pattern. White key 1 X (the clean sound),
2 ? (a random one), 3-14 slots 0-B: tap loads into the cursor step, SEQ + tap into the whole track, REC + tap into
every track of that kind; SAVE + tap stores the cursor step's sound.

Everything is kept by itself when the FM-1 is stopped and left alone for 10 s (at most once a minute).

## From FMS to the FM-1

What changed to fit the hardware, and why:

- **Slots.** FMS keeps 8 banks of 16 patterns per track. The FM-1 keeps 12 KB through power-off, and a pattern of
  sixteen full sounds is 370 bytes, so 7F keeps the live pattern and five slots per track (11.2 KB with the palette).
  BPM and scale are global rather than stored per bank.
- **Editing.** FMS edits with a cursor and two hold buttons. Here each step is a key and each setting a knob, so
  hold-and-turn replaces the cursor for most edits; SELECT still moves a cursor.
- **Sync.** MIDI clock in and out over USB and the TRS jack, in place of the link cable, analog and audio clocks.
- **Added.** A GBA DAC mode (8 bits at 32 kHz) for the console's grain, and a light theme.
- **Left out.** Bank names, the queued tempo change, and the button-layout options.

## For the FM-1

At most five voices sound at once (four FM, one noise; chords and echoes reuse them), so the load stays low: in a
side-by-side test on a computer, 7F's sound takes about a seventh of the time ORBIT DRIFT's does. SYSTEM (GLO, third
page) shows the CPU on the hardware.

## Develop

Only Docker is needed:

```sh
./fm1 dev          # http://localhost:8000: the emulator, the installer, the rescue; rebuilds when you save
./fm1 build        # build/7F-VERSION.fwsc
```

The firmware is in `src/main.c`; what it can call is in [platform/fm1.h](platform/fm1.h). Every push to `main`
publishes the emulator and the installer to GitHub Pages; a tag (`v0.1`) also makes a release with the `.fwsc`.

Installing firmware is at your own risk; back up your FM-1 first. If it does not start, hold OCT- while switching it
on (the USB rescue), then install from the page.

## License

GPL-3.0-only. Credits and third-party parts: [LICENSING.md](LICENSING.md).
