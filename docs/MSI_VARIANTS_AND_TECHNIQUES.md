# MSI Plessey decoding: variants, techniques, and field notes

This document captures what was learned building and hardening the MSI
Plessey decoder in this repository against real grocery shelf-label
photography. It is intentionally detailed: most of the hard-won knowledge
lives in failure modes that are invisible from the specification.

---

## 1. The specification (baseline)

MSI (Modified Plessey) encodes decimal digits only. In the "textbook"
form (Wikipedia / most tooling):

- A bit is a bar/space pair totalling **3 modules**: `0 = bar1+space2`,
  `1 = bar2+space1`.
- A digit is 4 bits (BCD), most significant bit first, = 12 modules.
- Start sentinel `110` (wide bar, narrow space).
- Stop sentinel `1001` (narrow bar, wide space, narrow bar).
- Quiet zones on both sides (typically >= 10 modules in print).
- Optional checksums: Mod 10 (Luhn), Mod 11 (IBM, weights 2..7 from the
  right), Mod 1010 (two chained Mod 10s), Mod 1110 (Mod 11 then Mod 10).
  MSI has no mandatory checksum; all of the above are opt-in.

Digit module maps (1 = black module):

```
0 100100100100   5 100110100110
1 100100100110   6 100110110100
2 100100110100   7 100110110110
3 100100110110   8 110100100100
4 100110100100   9 110100100110
```

## 2. Variant A vs Variant B: there are (at least) two real-world prints

The single most important discovery: **labels in the wild do not all use
the textbook 3-module bit.**

### Variant A -- "textbook" (3-module bits)

- `0 = bar1+space2`, `1 = bar2+space1`, bit total 3 modules.
- Start `110`, stop `1001`.
- Seen in: the older shelf-label generation (e.g. the `0828147`-class
  labels).

### Variant B -- "wide-space" (4-module bits)

- `0 = bar1+space3`, `1 = bar2+space2`, bit total **4 modules**.
- Start = bar2+space2 (module string `1100`).
- Stop = bar1+space3+bar1 (module string `10001`).
- Seen in: the current Publix shelf-label family (all of the large photo
  corpus used in development).

A decoder that only implements Variant A cannot read Variant B at all --
the pairing shifts and the stop pattern never matches. Conversely, a
decoder must not key stop detection on `space >= 3 modules` alone when
Variant B is in play: in Variant B **every zero bit contains a 3-module
space**, so a naive "3 modules of white after a narrow bar = stop + quiet"
rule false-stops after every other bit. Quiet zones must be required to be
strictly wider than any data space (we use >= 3.8 modules; Variant B's
data spaces reach 3 modules and after erosion can smear wider).

Practical rule that decodes both: classify a bit by the **bar width**
(`bit = bar > 1.5 module`) and validate the pair primarily by its **sum**
(2.6..4.4 modules) plus loose space bounds, with the running module
estimate updated from `sum / 3` or `sum / 4` adaptively.

## 3. Leading zeros are checksum-invisible

MSI checksum weights are anchored at the **right** end. Prepending zeros
adds zero-weighted digits, so:

- `082659368` and `82659368` (and `2659368`...) all validate identically.
- The bars alone cannot tell you how many leading zeros were printed.

Consequence: two zero-variants of the same code are equally "valid"
reads. The decoder must **collapse zero-prefixed variants of the same
symbol** (keep the longest voted form) before comparing candidates, or
votes split across `094438702` / `94438702` / `4438702` and no single
string reaches the acceptance threshold.

## 4. Real-world print and capture quirks

### 4.1 Ink spread erodes bars, widens spaces

On several label generations the printed narrow bar measures ~0.5-0.8
module while the following space measures 2.2-2.6 modules (Variant A).
Pair sums stay at ~3 modules, but a per-element width check
`bar ~= 1 module` rejects genuine reads. **Validate pairs by their sum
and classify the bit by the bar's relative width; never require the bar
itself to measure 1 module.**

### 4.2 Eaten spaces (blur fusion)

At ~1.5-2 px/module with blur, narrow spaces can vanish entirely: the run
stream loses an element (`bar space bar` becomes one wide black run).
A greedy (bar, space) pairing then misaligns for the rest of the line.
Recovery used here:

- **Split-bar repair**: when a pair fails validation and the space is
  < 0.6 module while the next run is black, merge bar+space+bar into one
  bar and retry the same bit (max 2 repairs per candidate).
- **Beam-search element alignment** (last resort): enumerate per-run
  interpretations (`[bar]` or `[bar, eaten-space, bar]` with the eaten
  space contributing any class 0..3 modules), beam-prune by width error,
  and validate only via start + stop + checksum. This is the only
  mechanism that recovers runs where several spaces were fully eaten.

### 4.3 Faded/low-contrast bars

Some captures have bar pixels at 130-180 on a 210-250 background. A
min/max midpoint threshold still classifies them, but the *strip's
context* matters enormously (see 4.4). Template correlation
(shape-matched, contrast-normalized) reads these where run thresholds
fail.

### 4.4 Category-defining trap: threshold pollution

The most damaging failure mode found: **the adaptive (min/max) threshold
of a scan line is global to that line**. A line that crosses the barcode
plus any dark graphic (price text, DSD tag, hole-punch shadow, gray
plastic shelf clip) has its threshold pulled by the graphic, and the
barcode region binarizes wrongly. The same pixels decode perfectly when
the line is cropped to the barcode's neighborhood.

This produces the signature symptom: **the crop decodes, the full frame
does not**. Mitigations, in increasing power:

1. Local adaptive binarization (windowed min/max, +/-28 px) per scan
   line. Fixes cases where the polluting graphic is far (> 30 px) from
   the barcode.
2. Horizontal sub-windows of the scan line (sliding windows, 1/3..2/3 of
   the width). Fixes cases where the graphic is adjacent.
3. **Dark-pixel-density localization**: count pixels < 140 per row;
   barcode rows carry ~2-4x more dark pixels than text rows in the same
   image. Take the densest band, derive its horizontal extent from
   per-column dark counts, crop with margins, decode the crop. This is
   the generic version of "crop to the barcode" and is what makes
   graphic-adjacent captures tractable without knowing the barcode
   location a priori.

### 4.5 Razor-thin decode windows

For the smallest captures (~1.3 px/module), decodability is
phase-sensitive: a 12-14 row strip decodes at x=147 but not x=144 or
x=153. Root causes observed: (a) the left quiet zone is only ~13 px
(~3.8m) so a window shifted right truncates it and the start candidate
is rejected; (b) a window shifted left exposes an extra dark element
(a clip edge) that a first-success decoder latches onto, shadowing the
true start. Mitigations: dense strip grids (x step ~4-6 px, y step ~4,
strip heights 10-14 rows) and **longest-read-per-line** decoding so an
artifact start cannot shadow a longer true read on the same line.

### 4.6 Scanner-visible artifacts

- A 1 px white speck inside a wide bar and black specks inside spaces
  (repair handles the first).
- Hole-punch circles and shelf-clip plastic: bright or mid-gray, they
  mostly matter through threshold pollution (4.4).
- Photos of labels behind transparent clips: the clip adds a gray border
  whose columns have *low* "dark" density -- the dark-density localizer
  ignores them.

## 5. Decoder architecture as implemented

Pipeline per frame (Auto checksum mode):

1. **Band localization** (`findBands`): per-row adaptive-threshold
   transition counts; contiguous dense rows form bands (min height 10,
   relative threshold at 35% of peak, absolute floor for soft prints).
2. **Band tier**: averaged profile of the band's middle quarter + rows at
   a stride; each line through the run decoder and (budgeted) template
   correlation. Early exit when `minVotes` (2) agreeing readings found
   (explicit checksum modes only).
3. **Full-row coverage tier** and **column tier** (vertical barcodes).
4. **Vote pooling** per band-isolated and pooled; adjacent-row duplicate
   suppression (correlated rows must not each count as a vote).
5. **Leading-zero collapse** before winner selection.
6. **Winner selection**: votes first, then checksum strength (Mod 1010 /
   Mod 1110 count double-check and outrank single-check variants on
   near-ties), then length; double-check single-vote reads are accepted
   only under strict gates.
7. **Weak-result cascade** (all strictly additive; a route can only
   replace a result with a verifiably stronger one):
   - isolation re-decode of the top bands' crops (larger margins),
   - 2x nearest-neighbour upscale retry (frames < 800 px long edge),
   - contrast-stretch retry,
   - **rescue route**: local-threshold rows, dark-density localized
     crops, and a strip scanner (10-14 row strips, multi-scale windows,
     fine grids) guarded by conservative acceptance (>= 4 agreeing
     votes, >= 7 digits) to keep false positives out.

### Speed and budgets

Measured on Apple Silicon, `-O3`:

- clean frame with a barcode: ~0.8 ms (1280x720 Y plane)
- barcode-free frame, full scan: ~2 ms
- pathological rescued frames: 0.1-1 s (last-resort routes only)

The decoder is thread-safe, allocation-light, and does its work on the
camera's Y plane directly (zero-copy JNI; offset + row stride honored).

## 6. Checksums as a filter (what they can and cannot do)

- `Checksum` enum: Mod 10, Mod 11, Mod 16, Mod 43, Mod 47, Mod 103,
  Mod 1010, Mod 1110. Only Mod 10/11/1010/1110 are meaningful for MSI.
- A random 8-9 digit string satisfies Mod 10 by chance == 1/10, Mod 1010
  == 1/100. Vote agreement (>= 2 independent scan lines) is the real
  filter; strength breaks ties.
- Because leading zeros are invisible (section 3), checksums cannot
  disambiguate zero counts -- only the bars can, and when they can't, the
  longest voted form is the least-surprising answer for the shelf-label
  domain (all SKUs are zero-prefixed 7-digit payloads + 1-2 check digits).
- Reject the degenerate uniform string (all digits equal): blank surfaces
  and glare bands decode to such strings and pass Mod 10 by chance.

## 7. Evaluation

- **Corpus A** (116 modern shelf-label photos, including glare, clips,
  hole punches, tiny modules): 115/116 found+valid; 12/12 of the
  ground-truthed payload files exact; 6/7 independently verified ground
  truths exact; the single miss is a capture explicitly rated as
  non-realistic by the collector.
- **Corpus B** (6 older shelf-label photos, modules ~1.3-2.5 px,
  Variant A, several behind shelf clips): 3/6 exact (including a capture
  that required the strip-scan rescue at 1.3 px/module). The three
  failures are captures whose decodable windows are razor-thin (section
  4.5) and where consistent phantom readings out-vote the truth in
  single-frame decoding. They are honest targets for future work:
  multi-frame voting (shoot the label twice and require agreement across
  frames) is the natural next step and is how production scanners solve
  this class.

## 8. Testing

`tools/msi_selftest.cpp` runs 34 host tests: the specification fixture
(Wikipedia module map + check digit example), all four checksum modes,
both print variants, subpixel/repair/window edge cases, and negative
tests (blank, noise -- across multiple deterministic noise samples).
`tools/msi_bench.cpp` measures per-frame latency.
`tools/msi_decode_image.cpp` decodes BMPs with JSON output and
`MSI_DEBUG=<n>` trace levels.

No barcode images are included in this repository; the corpora are
private.
