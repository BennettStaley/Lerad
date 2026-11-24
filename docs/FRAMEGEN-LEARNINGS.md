<!-- SPDX-License-Identifier: MIT -->
# Frame generation: what was tried, measured and learned

This is the history behind Lerad's frame generation: how the method got to
where it is, how it was tested, the research rounds (including the
monocular-depth experiment), and the dead ends, so they are not walked
again. How the shipped version works, its switches and its build are in
[src/framegen/README.md](../src/framegen/README.md); this file does not
repeat that.

The work was done October 2026 in the prototype (MPV-SW-Capture with a
ReShade add-on, `zz-msc-framegen.addon64`) and ported to Glasir unchanged
(`fg_eval` scores identical, 27 shader headers byte-identical). Folders and
scripts named below that are not in this repo live on the owner's PC; the
captures (real game frames) are never committed.

## Contents

1. [Goal and constraints](#1-goal-and-constraints)
2. [How the method evolved](#2-how-the-method-evolved)
3. [How quality is measured](#3-how-quality-is-measured)
4. [Research rounds](#4-research-rounds)
5. [The monocular-depth experiment](#5-the-monocular-depth-experiment)
6. [Pacing, VRR and latency](#6-pacing-vrr-and-latency)
7. [Dedup and the PS5's noise](#7-dedup-and-the-ps5s-noise)
8. [The GPU budget](#8-the-gpu-budget)
9. [Testing: tools and gotchas](#9-testing-tools-and-gotchas)
10. [Known leftovers and open ideas](#10-known-leftovers-and-open-ideas)
11. [More than x2, screens without VRR, honest numbers (11-12)](#11-more-than-x2-screens-without-vrr-honest-numbers-11-12)
12. [Other frame generators](#12-other-frame-generators)
13. [The GPU makers' frame generation, run on video](#13-the-gpu-makers-frame-generation-run-on-video)
14. [The HUD found by itself (11-18)](#14-the-hud-found-by-itself-11-18)
15. [The menu on a slow picture (11-18)](#15-the-menu-on-a-slow-picture-11-18)

---

## 1. Goal and constraints

- x2 only: 30 → 60 and 60 → 120 fps (the owner's call). x3 and x4 were
  added on 11-12 as an option, never the default (section 11).
- Two frames, nothing else: a capture card gives no motion vectors, no depth,
  no HUD mask. The add-on finds its own motion. (It uses no ReShade motion
  vectors either, which was once suspected of causing a ghost.)
- Runs inside mpv's D3D11 device as a ReShade add-on, after DLSS 5, on the
  presented picture (so it sees what DLSS 5 made, and the menu is drawn after
  it).
- Any GPU (it is plain D3D11 compute and pixel shaders, SM 5).
- Budget agreed with the owner: up to ~5 ms GPU per generated frame; a
  lighter mode for 60 → 120 is fine.
- Behaviour agreed with the owner for motion that cannot be found (a turning
  spiky hairdo): show it **solid with a lagging shape** (one real frame moved
  with the head), never see-through, never a blend.

## 2. How the method evolved

Each step below fixed something seen in the owner's captures (F9 saves five
real frames and the frames drawn between them) or in the synthetic clips.
Scores are `fg_eval` means at the time (section 3).

### 2.1 The base: motion-compensated, not blended

- A plain cross-fade scored 23.6 dB on the synthetic clips; motion
  compensation 43.0 dB (2026-10-25).
- Luma pyramids of both frames, block search coarse to fine (16 px cells),
  median filtering, then a per-pixel mix of candidate motions from the 3x3
  neighbouring cells, each scored on a 5-pixel cross.
- Handled from the start: fades (a brightness gain from motion-matched
  content), cuts (repeat frame A when more than 20% of cells match nothing),
  static overlays, very small windows (blend fallback).
- Finer 8 px cells were tried and scored worse.

### 2.2 Blockiness around characters and effects (10-25)

- Seen: blocky patches where things changed at 16 px cell edges.
- Cause: the candidate set shifting at cell edges, and a per-block one-frame
  cost.
- Fixed with a tent fade of each cell's reach, a bilinear field mismatch, and
  a wide soft mix (`soft_wide` 0.15) where no one-frame option explains the
  pixel.
- `eval/blockiness.py` (1.0 = no cell grid) on the owner's pairs: 1.18 / 1.55
  max before, 1.00 / 1.04 after; real frames score 1.04.
- Dropped: a cell-cost "doubt" fallback to cross-fade or hold (no visible
  gain).

### 2.3 A copy of the head beside a still character during camera orbits (10-25)

- Seen: a faint hair and face copy about 50 px beside the head.
- Cause: the head, a few blocks wide, was lost to the scenery on the coarse
  pyramid levels, and the fine search radius could not get it back.
- Fix: a second search around zero motion, taken when its best cost beats the
  estimate window's best by a margin (+0.19 ms).
- Dead ends:
  - comparing zero against the *unsearched* estimate punched a sky hole into
    an evenly textured pillar;
  - an absolute fit limit, or fewer pyramid levels, traded the pillar against
    the head.

### 2.4 Background showing through the top of the head (10-26)

- Seen: during camera turns, the background showed through the top of the
  head.
- Cause: turning hair spikes fit themselves worse (5-tap cost ~0.05) than the
  plain ground beside them carried by the scenery's motion (~0.01).
- Fix ("persistence", `layer_cost` 0.2): where both frames show the same
  thing at a pixel and a still cell is near, a moving option that brings a
  different colour pays a penalty.
- Dead ends:
  - global camera detectors (dominant motion, still share): they cannot tell
    static-camera combat from a camera turn;
  - a forward/backward round-trip "hidden motion" cue (the fields have a halo
    of the character's zero motion);
  - a proportional penalty;
  - an unconditional penalty, which erased details narrower than a pan's step
    (−2 to −15 dB on pans).

### 2.5 "Three tips" in fast orbits (10-28)

- Seen: at 130–140 px/frame, a faint copy of the hair tips half a scenery
  step beside the head.
- Cause: in the strip that only one frame shows, the wrong frame's sample
  lands on the character and costs the same as a real occlusion.
- Fix (`slow_cost`, NearSlowBlock): a one-frame option along the motion most
  cells share pays when its sample is near a slow block of its own frame,
  applied one way round only.
- The 25-block loop must stay `[loop]`: unrolled, it tripled the shader and
  cost ~0.5 ms.
- Dead ends:
  - a symmetric version blocked one-frame options in smoke and effects
    (patches on spear blades);
  - "still = slower than 0.1x the fastest cell" tore a colonnade and holed a
    dress.

### 2.6 The core weakness, and the character layer (10-28 to 10-30)

A thin, deforming spike over smooth floor loses to the floor's motion, which
fits almost perfectly. This is a property of the method, not a regression; it
is what the research rounds (section 4) were about. What shipped:

- **The character layer (LAYERS).** A local affine background model per
  128 px block (a global model failed on perspective floors), then the
  character found as a compact component that moves differently, its own
  motion searched, drawn hard from frame A, replacing only the pixels that
  WarpPS drew with background motion.
  - needle 15/15, needle ratio 0.40 → 0.99; everything else unchanged;
    +0.07 ms on pans, +0.75 ms on orbits.
- **Polish rounds:**
  - `layer_bgnear` 0.3: the gate counts options near the background model,
    for floors whose motion is 10–15 px off the model near the head;
  - selection gates that filter components instead of killing the layer;
  - flat cells weighted into the background model (motion-blurred floors
    have no detail);
  - a ghost rule (off-layer options landing on the layer's mask pay).
- **"pick3" (10-30), for NPCs and capes winning over the player's head:**
  merge small clear fragments into slow central cores, prefer still and
  central components, refine the edge.
  - Only active when the dominant motion is 6 px or more, so fights stay
    bit-identical.
  - hole score 4930 → 4558; everything else equal.
- **Better cut detection.** A real cut read 0.206 against a 0.2 threshold.
  The detector now also counts the unmatched share among detailed cells: the
  real cut reads 0.54, the busiest non-cut 0.34.

Dead ends in this phase:

- **Per-pixel colour ranking** (round-1 implementations):
  - minority-vector "rescue" pasted stair and pillar aliases;
  - contrast priority gave tiny gains or streaks;
  - keeping the spike vector through the median gave a ragged spike at
    −1.8 dB on the synthetic clips.
  - Lesson: per-pixel ranking cannot restore a deforming spike; it needs
    region/layer decisions.
- **Dense colour-guided motion with splatting ("guide"):** needle 15/15, but
  splat specks and tears almost everywhere; dropped after review.
- **A "near layer"** (second-search motion as extra per-pixel candidates):
  +4 ms, −1.9 dB.
- **Per-pixel stand-still detection:** noisy speckle, 13 ms.
- **Up to 3 layers ("multi3"):** +0.4–0.7 ms, hard pastes.
- **Coarse neighbours' estimates:** fixed heads slightly but spread aliased
  motion on stairs, washed a minimap corner, doubled a label (found by a
  17-agent review of all pairs).
- **A "pair" penalty:** gains only on easy pans.
- **`cap_fit` / `cap_cell` softness caps:** they wiped the faint ghost of a
  fast spear and thinned armour in combat; off by default.

## 3. How quality is measured

### 3.1 Tools (in `src/framegen/eval`)

- **`fg_eval.exe`** runs the add-on's own shaders offline.
  - Synthetic clips, made from game screenshots: pans, zoom, subtitles,
    fades, a cut, HUD, UI fade/slide, a noisy HUD, orbiting sprites. Scored as
    PSNR against the true middle frame.
  - Real captures: scored skip-one (frames 1 + 3 → 2), or `--adjacent` to
    write what the add-on would draw.
- **Metrics:**
  - `blockiness.py`: the cell grid; 1.0 = none.
  - `head_score.py`: PSNR in 22 head regions.
  - `needle_score.py`: thin hair pixels kept against both real frames.
  - `hud_score.py`: how far the frames in between are from what stands
    still in all five real frames of a capture (a HUD over the scenery).
  - `hole_score.py` (in the research folder on the owner's PC, not in this
    repo): hair dark in both real frames but not in the generated one.
  - Ghost counts.
- **Viewing:** `compare.sh` (tone-mapped crops side by side) and
  `view_captures.py` (a capture viewer).
- **`debug_view`** in the add-on colours pixels by what they show:
  - 1: which option was used;
  - 2 / 3: softness / cell cost;
  - 4: motion lengths (the quickest way to see a lost object);
  - 10: the strongest option's motion;
  - 11: depth, in the depth build.

### 3.2 Lessons about measuring

- Mean PSNR over-rewards easy clips (above 45 dB); judge by the hard clips,
  the captures and blockiness.
- Skip-one head PSNR is a poor proxy for hair, because skipping a frame
  doubles the motion. The adjacent-pair needle score replaced it.
- **Never tune to one capture.** The owner had one "dodge" capture deleted
  everywhere because work was being judged by it. Judge across the whole set:
  totals, plus how many captures got better and how many worse.
- Live `between_*.png` from F9 match `fg_eval` exactly, so offline results
  are what the player sees.
- Visual review by independent judges (Grok 4.7, several tasks, all pairs,
  before vs after) caught what scores missed: the combat regressions of
  `cap_fit`, the stair blotches, tears in effects. Every shipped change went
  through one.

### 3.3 Where it ended (final prototype build, 10-30, ported unchanged)

| measure | value |
|---|---|
| synthetic clips (mean PSNR) | 42.93 dB (44.08 with 4.8's defaults) |
| captures, skip-one | 32.75 (28 captures) / 31.43 (40) |
| head regions | ~27.9–28.3 dB (set-dependent) |
| blockiness mean / max | 1.069 / 1.152 |
| needle pass, ratio | 15/15, 0.97–0.99 |
| hole score | 4558 |
| GPU time per generated frame | ~3–3.7 ms at 1440p, RTX 5070 Ti |

## 4. Research rounds

Five research reports were written first, then two rounds of
implementations in private copies, scored with the same tools.

### 4.1 Hardware and SDK motion, rejected

- **NVIDIA Optical Flow (NVOFA) and NvOFFRUC:**
  - a finer hardware grid does not change the tie the floor wins;
  - NvOFFRUC is over budget at 1440p, destroys HDR, its hole fill is the
    phasing bug itself, and shipping `NvFRUC.dll` is not granted by the
    licence text.
  - The only acceptable experiment would be OFA 4x4 Medium feeding the
    existing per-pixel step (3–5 ms, 30 → 60 only). Not done.
- **D3D12 Video Motion Estimation:** 8x8/16x16 encoder search through a
  second device, unknown radius; no finer than ours.
- **AMD FidelityFX optical flow and frame interpolation (MIT shaders):**
  - its optical flow is the same design as our search;
  - its interpolation keeps thin parts only by using depth.
  - FSR 4 frame generation (ML) needs camera data and Radeon 9000.
  - AMD's AFMF has no source.
- **Neural interpolation:**
  - RIFE-class networks need ~16–26 ms at 1440p, far over budget;
  - they are trained on photometric error, the same tie the shader loses.
- **NVIDIA Maxine Video Frame Generation (VFG):**
  - RTX 40/50 only, 2–8x, Low/Medium/High;
  - CUDA, so D3D11 ↔ CUDA sharing would be needed;
  - the runtime (~435 MB) is anonymous from NVIDIA's Python index, but the C
    headers need an NGC account;
  - cost on an RTX 5070 (with Python overhead): Medium ~2.9 ms at 1080p and
    ~3.9 ms at 4K; High ~14.6 ms at 1080p;
  - users report edge "jelly" on Low/Medium in camera turns.
  - Not built. The suggested first step, if ever: score it offline with
    `fg_eval` on the same captures.

### 4.2 Literature on motion-compensated interpolation

The consensus was per-pixel candidate ranking with foreground priority. The
round-1 implementations of that failed (section 2.6). The useful outcomes:

- the critic's verdict to stamp the body region from one frame;
- the needle score;
- the layer design that shipped.

### 4.3 A 1-3-3-1 pyramid filter, rejected (11-14)

- **The idea:** each luma pyramid level is the 2x2 mean of the one above. A
  2x2 mean lets through much of what is just too fine for the next level,
  where it shows as a coarse pattern the picture never had. DLSS5-Feeder
  2.0's own estimator switched to 1-3-3-1 on each axis for that reason
  (brickwork read as moving the wrong way), and stairs and pillars were
  recurring trouble here (section 2.6).
- **Tried** as an evaluator switch (`pyr_filter`: every level, or from
  level 1 down only), offline with `fg_eval` on the 18 clips and the 32
  captures in the prototype's `framegen-captures`. Not kept in the code.

| measure | 2x2 mean | 1-3-3-1, every level | 1-3-3-1, level 1 down |
|---|---|---|---|
| synthetic clips (mean PSNR) | 42.93 dB | 42.82 | 42.70 |
| captures, skip-one (32) | 30.35 dB | 30.41 | 30.42 |
| captures better / worse (beyond 0.03 dB) | - | 10 / 13 | 12 / 14 |
| blockiness, adjacent frames (mean) | 1.065 | not run | 1.064 |

- **The mean gain was one capture.** A staircase (20261025-124130-247)
  gained 2.2 dB skip-one: the patch where the wrong step was matched shrank
  to about a third. The other 31 moved by 0.25 dB or less and cancelled out.
- **In real use the stairs got worse.** On the adjacent pairs of that
  capture the 2x2 mean draws the stairs clean, and 1-3-3-1 added a patch on
  two of the four. The skip-one gain was the doubled motion (section 3.2).
- **Clips:** the orbits gained 0.3-0.4 dB; `pan_fast` lost 0.7, `ui_fade`
  0.3-0.5, `pan_1440` 2.2.
- **A retune did not rescue it.** `search_lambda` 0.005-0.04 and
  `guess_cost` 0.35 / 0.7 with the new filter: no setting kept clips,
  captures and stairs all at least as good as the 2x2 mean. A lower
  `search_lambda` cleaned the stairs, but it does that with the 2x2 mean as
  well (section 4.4).
- **Lesson:** the search's thresholds and the 2x2 mean were tuned together
  on these captures; a filter that is better on paper only moves which
  frames get the patches. Judge such a change on adjacent pairs, not on
  skip-one PSNR.

### 4.4 `search_lambda` 0.01, rejected (11-15)

- **The idea:** it came up as a control in 4.3. Half the default (0.02) gave
  16 of 32 captures better and 5 worse skip-one.
- **Tried** offline with `fg_eval`, same 18 clips and 32 captures.

| measure | 0.02 | 0.01 |
|---|---|---|
| synthetic clips (mean PSNR) | 42.93 dB | 42.79 (12 of 18 worse, 2 better) |
| captures, skip-one (32) | 30.35 dB | 30.41 (16 better, 5 worse) |
| head regions, skip-one (24) | 26.32 dB | 26.15 (one head −3.4 dB) |
| blockiness, adjacent frames (mean / max) | 1.065 / 1.141 | 1.064 / 1.145 |
| needle pass, ratio (3 pairs left in the set) | 3/3, 0.98 | 3/3, 0.97 |

- **On adjacent pairs it is worse.** Of the 20 regions that differ most
  between the two (at most two per capture, each beside its real frames),
  about 8 were worse with 0.01, 3 better, the rest the same. Of the 8 heads
  that differ most, none was better and 3-4 were slightly worse.
- **What it does:** hard torn pieces where 0.02 gives a soft ghost - a
  walking NPC's torso drawn twice, dark blocks in fire and sparks at the
  picture's top edge, flecks beside hair, a third copy of a lantern at the
  picture's edge. The few gains were doubled NPCs at the picture's edge
  becoming one.
- **Lesson:** the same as 4.3. The skip-one gain was the doubled motion,
  where a freer search finds more; at real distances it only breaks up what
  was soft.

### 4.5 The last pair's motion as a third search centre (11-15)

In the add-on it is a switch, off by default - the one test left of the
four (4.8): `HistorySearch=1` in `ReShade.ini`, `[LERAD_FRAMEGEN]` (the
evaluator's is `--set hist_search 1`). The owner has run it with the card
since 11-15 (below).

- **The idea:** the search tries two centres per block, the coarser level's
  estimate and no motion. A third: what the block did over the pair before
  (the last pair's b → a field, kept per level; a's block is assumed to go on
  as it came, b's block is looked up where it was in a). It is the only
  information the two frames do not hold. What that search finds is taken by
  the rule the search around no motion already has (better by 0.5 luma SAD
  and at most half as badly: `hist_cost`, `hist_ratio`).
- **In the code:** `Interpolator::keep_history()` after a pair is drawn,
  `DrawOptions::follows` on the next pair when its frame a was that pair's
  frame b. `fg_eval` does both within a clip or capture. With the switch off
  the frames are bit-identical to before.
- **In the add-on:** the history is used only when the present before drew
  a frame in between and both pairs came at the source's pace (0.5-1.5
  intervals); a skipped frame in between, a late frame, an OSD redraw or a
  resize drops it. `ReShade.log` says `HistorySearch=1: searching around the
  last pair's motion as well` the first time it is used. Checked without a
  card on a copy of the app folder (`tools/smoke_raw.py`, 11-15): 30 real +
  29.5 generated fps with the switch off and on, no drops after start-up.
  The test pattern says nothing about quality: its frames come out the same
  either way.
- **Coverage:** in the captures only the second skip-one pair and three of
  the four adjacent pairs have a pair before them; live, every pair would.

| measure | off | on |
|---|---|---|
| synthetic clips (mean PSNR) | 42.93 dB | 43.04 (8 better, 0 worse) |
| captures, skip-one (32) | 30.35 dB | 30.40 (15 better, 0 worse) |
| head regions, skip-one (24) | 26.32 dB | 26.38 |
| blockiness, adjacent frames (mean / max) | 1.065 / 1.141 | 1.065 / 1.141 |
| needle pass, ratio (3 pairs left in the set) | 3/3, 0.98 | 3/3, 0.97 |
| skip-one pixels it changes: error to the real frame | 22.6 of 255 | 16.9 (22 captures closer, 4 further) |

- **What it does:** during camera turns, things at the picture's edges and
  NPCs walking past - trees, pillars, a banner, a person drawn twice or
  see-through before - come out single and solid. The staircase capture is
  untouched.
- **Blind review of the adjacent pairs** (a fresh Claude agent, the 40
  regions that differ most, at most two per capture, old and new in random
  order beside the real frames): new better in 19 (4 sure, 8 fairly, 7
  barely), old better in 9 (0 / 3 / 6), no difference in 12. An earlier
  look at the top 20, knowing which was which, gave about 11 / 3 / 6.
- **Where it loses:** the four captures further from the real frame are three
  fights full of effects (skip-one: a few darker, harder pieces inside an
  explosion and around a spear hand, where it was a soft smear) and one
  banner beside the minimap (broken either way). On the adjacent pairs the
  reviewer's three fairly sure losses were a red banner on a pillar leaving
  the picture (a torn dark piece instead of a faint ghost), a pedestal at
  the picture's edge, and two small HUD digits over fast dark scenery with a
  little more fuzz beside them (another game, 20261031-144655-466).
- **Tried on top:** not letting it replace what the search around no motion
  won. No frame changed visibly, so that is not how the digits got their
  fuzz; left out.
- **Thresholds:** looser ones score higher skip-one and then start to lose
  (`hist_cost` 0 / `hist_ratio` 1: 24 captures better, 4 worse) - the trap of
  4.3 and 4.4. The search around no motion's own values lose nothing, so they
  stay; they were not tuned on these captures.
- **Cost:** not measurable in the evaluator (single runs vary by 1 ms on a
  busy GPU); one more search per block where the third centre differs from
  the other two, so at most half again of the search's ~0.5 ms.
- **First captures with the card** (11-15, the owner, `HistorySearch=1`,
  five F9 captures of another game: a dark cave, running and fast camera
  turns with heavy motion blur):
  - the add-on's own frames match the evaluator's with the switch on (pairs
    2-4: 0.002-0.14 % of pixels apart, against 0.02-0.78 % from the
    evaluator with it off), so the wiring does what was measured offline;
  - those live frames had minutes of pairs behind them, the evaluator's one
    pair: a long run of history did not drift away from a short one;
  - on this game it changes little: under 0.8 % of a frame's pixels,
    skip-one +0.00 to +0.11 dB, and of the 12 regions that differ most 11
    look the same and one has a hard dark block at the picture's edge where
    both versions are broken. Nothing was clearly better here.
- **Not known:** what it costs on the GPU next to DLSS 5, and how it looks
  in play on the game the gains were measured on (camera turns with NPCs
  and scenery at the picture's edges).
- **A test clip to distrust:** `orbit_drift` scores 4 dB under `orbit`
  because ffmpeg puts the overlaid character on even pixels only: it moves 4
  px, then 2, not 3 and 3, and the "true" middle frame is 1 px off the
  half-way point. The layer finds its motion exactly (6.0 px).

### 4.6 The picture's edges, and the generator on the picture alone (11-15)

**Status:** on by default since 11-16 (4.8): `PictureOnly` and `EdgeRules`
(evaluator: `--picture X0 Y0 X1 Y1`; the edge rules are its defaults now,
`--set edge_cost -1 --set edge_explains 1` is without them - when this was
written they were off and `--set edge_cost 0.01 --set edge_explains 0` turned
them on). With both off, frames are bit-identical to before. The HUD they
cost is 4.7's matter (`StillHud`).

The outer 96 px of the picture were its weakest part: 25.6 dB skip-one on
the 32 captures against 28.6 dB for the rest. In a camera turn the strip
that only one frame shows (content leaving or coming in, half the step
wide) came out as a smear or a ghost of the other frame.

- **The guess that was wrong:** WarpPS's "off screen" test (`Inside`) knows
  the frame, not the picture, so a sample in a black bar counts as on
  screen. Testing against the picture's box instead changed nothing: edge
  strips 25.57 → 25.57 dB, the rest of the frame bit-identical. A sample in
  a bar is already ruled out by its cost.
- **What does go wrong** (seen with `debug_view` 4 on a 165 px turn):
  - *SearchCS, with bars:* a coarse block that straddles a bar is held
    still by the bar's part of it, and the finer levels under it start from
    no motion, 80 px short. Everything within a coarse block of a bar is
    pulled toward standing still.
  - *MidCS:* it compares a with b along each motion on offer. The right
    motion for the strip takes one of the two samples off the picture,
    where it reads the bar or (at the frame's own edge) the edge pixel
    repeated: a high cost, so the cell takes no motion or a wrong one.
  - *WarpPS, at the frame's own edge only:* where the other frame's sample
    is off the frame, one frame alone "explains" the pixel whatever that
    frame's motion says, so the softness stays narrow and the pixel makes a
    hard choice between whatever motions are on offer: torn pieces where a
    bar gave a soft fade.

**PictureOnly: the generator on the picture alone.** `fg::Interpolator`
takes the picture's place (`fg::Picture`): it copies that part of a and b,
runs every pass on the copies, draws into that part of the target and
copies what is around it from a. The add-on has the box from mpv
(`osd-dimensions`, as the screenshot does) and makes the generator anew
once the box has been the same for 15 presents.

- The box must be the picture exactly (x = 440–3000 here). One or two
  columns of the picture's edge spill into the bars at a fifth of its
  brightness; inside the box that column is carried along a turn as a dark
  line. Outside it, it is drawn as frame a has it.
- `cut_fraction` was set on frames with bars, whose cells always match. Of
  the picture alone the same frames leave a larger share unmatched: one
  fight frame at 0.172 of the frame is 0.231 of its picture, over the 0.2
  limit, and was shown as a repeat (−1.9 dB on that frame). The limit is
  now scaled by frame area over picture area, which keeps the decisions as
  they were.

**EdgeRules**, for the frame's own edges (the picture's, with PictureOnly):

- mid: under a motion that takes a cell's content off the frame, a pixel
  whose sample in a or b is off the frame costs a fixed `edge_cost` (0.01)
  instead of the comparison with the edge pixel, if
  - the frame further in, at the nearest place along the motion that both
    frames show, has that motion (`edge_tol`: within a quarter of its
    length + 2 px) and it carries a onto b there (`edge_fit` 0.02);
  - the pixel does not stand still (`edge_still` 0.004: a HUD at the edge
    is not carried off with the scenery).
- warp: `edge_explains` 0, so a one-frame option explains a pixel at the
  frame's edge only if that frame's own motion agrees, as everywhere else.
- Not in the switch: `edge_search` (a block the coarser estimate takes off
  the frame keeps the estimate, unless it matched within `edge_fit`). It
  adds another 0.5–1 dB on the pan clips, but in a blind review with it
  (and before `edge_still`) the two confident verdicts went against the
  rules: hard blocks across an effect scene and a broken minimap border.

| | baseline | PictureOnly | + EdgeRules |
|---|---|---|---|
| clips, mean | 42.93 dB | 42.93 (no bars) | 44.11 (`pan_slow` −0.39 at 60 dB, the rest up or equal) |
| captures, skip-one (32) | 30.35 dB | 30.87 (26 better, 3 worse) | 31.05 (27 better, 1 worse) |
| the owner's five of 11-15 | 30.71 dB | 30.94 | 31.02 |
| edge strips, 64 frames | 25.57 dB | 25.75 (32 better, 26 worse) | 26.31 (43 better, 14 worse) |
| rest of the picture | 28.62 dB | 29.07 | 29.14 |
| head regions | 26.32 dB | 26.25 | 26.25 |
| needle pass, ratio | 3/3, 0.98 | 3/3, 0.99 | 3/3, 0.99 |
| blockiness mean / max | 1.065 / 1.141 | 1.063 / 1.141 | 1.062 / 1.141 |
| evaluator GPU time per frame | 5.6 ms | 4.2 ms | 4.1 ms |

Blind reviews of adjacent pairs (40 regions each, at most two per capture,
which version is which randomised per region):

| old → new | where | new | old | same | confident (high) new / old |
|---|---|---|---|---|---|
| baseline → PictureOnly | whole picture | 29 | 9 | 2 | 7 / 2 |
| baseline → PictureOnly | within 320 px of the edges | 28 | 12 | 0 | 9 / 1 |
| PictureOnly → + EdgeRules | edges | 26 | 8 | 6 | 5 / 0 |
| baseline → both | whole picture | 27 | 10 | 3 | 12 / 1 |

- **What it loses, and why it stayed a test switch until 4.7:** the bars were
  protecting what stands still near them. In the cave game of 11-15 the HUD
  icons 30–140 px from the picture's left edge come out worse in most pairs
  of the three fast-moving captures (a faded icon, a doubled digit, blocks
  beside them); the two slow captures are unchanged. It was a confident
  loss in all three reviews against the baseline. On a 16:9 screen there are
  no bars, so that HUD is drawn this way there already. The edge rules do
  not repair it: thin or see-through HUD parts over fast scenery are not
  found still by the search, and the persistence that protects them
  (`near_still`) needs a still cell nearby, which the bar supplied.
- **Skip-one misleads at the edges:** it doubles the step, and a sharp strip
  a few pixels off scores under a smear. The frame the rules "lost" most on
  showed a clean streak against a torn patchwork. Judge edges on adjacent
  pairs.
- **Checked in the add-on** (no card: a 960x720 test pattern in a 1280x720
  window, bars of 160 px): the log's "PictureOnly=1: the picture is 960x720
  at 160,0", 30 real + 29.6–29.9 generated fps in SDR and HDR, the bars
  identical to frame a, and in HDR the add-on's own frames 0.001–0.06 % of
  pixels from the evaluator with the same switches (0.14–0.86 % from the
  evaluator with them off).
- **The switches while playing:** both (and `HistorySearch`,
  `DebugMarker`, `LogStats`) are checkboxes under "Lerad Frame Generation"
  on the Add-ons page of ReShade's overlay (Home), so one can be flipped on
  the scene in question. Tried on the test pattern: on, off and on again,
  the log's "PictureOnly=1: the picture is ..." / "PictureOnly=0: the whole
  1280x720 back buffer again" half a second after each (15 presents at
  30 fps), the marker at the picture's corner or the frame's accordingly.
  - Trying it found a fault of the first version: once made for a picture,
    the generator was never made for the whole frame again (the "box that
    failed" is all 0 when none did, which is also "the whole frame"). With
    the switch only in the file that needed the picture to grow to fill a
    back buffer of the same size; it would then have kept drawing into the
    old box.
- **With the card** (11-15 evening, the owner: 3440x1440 HDR, DLSS 5 on,
  `HistorySearch=1` as well, two games, five F9 captures with both
  switches on):
  - *In play:* "didn't seem worse". Both were flipped off and on twice from
    the panel in the first game (half a minute off in all); the second game, the
    cave game, was played with them on only. 30.0 real + 30.0 generated fps
    either way.
  - *The wiring:* the add-on's frames match the evaluator's with the
    switches on (pairs 2-4: 0.03-0.38 % of the picture's pixels apart,
    3-15 % from the evaluator with them off), the bars are frame a's.
  - *Skip-one:* 29.61 → 30.13 dB (3 captures better, 2 the same); edge
    strips 25.77 → 27.20 dB (9 of 10 frames better), the rest 28.43 →
    28.81.
  - *Blind review* of the 20 regions of the adjacent pairs that differ
    most (four per capture; by the author of the change, which is which
    read afterwards): new 7, old 9, same 4; confident 2 / 2. It splits by
    game:
    - the first (a temple; minimap, quest line and speech bubbles at the
      picture's edges): new 6, old 2, both barely. An NPC at the left edge
      whose half was a smear is whole, a speech bubble's garbled text can
      be read (skip-one there 20.6 / 26.3 → 24.6 / 32.5 dB);
    - the cave game: new 1, old 7. Four are the HUD icons at the left
      edge in all four pairs of one capture, the loss above (an icon gone
      or in pieces; skip-one in that corner -0.6 to -1.1 dB on one frame
      of each of the three captures, never better). Three are a drone in
      the middle of the picture, torn either way and within 0.1 dB
      skip-one: the moved block grid, not the switches.
  - So the loss is still there and was not seen in play. A default wants
    the HUD protected first: 4.7.
- **Not known:** the GPU time next to DLSS 5 (the generator works on 74 %
  of an ultrawide frame's pixels, plus two copies of the picture; the
  rates above show no difference), the cave game with the switches off
  and on in one session, and a window being resized (the box is followed,
  but only the test window's one change was tried).
- **Also learned:**
  - content-based bar detection does not work on these captures: the bars
    are a flat 0, 256 or 640 of 65535 depending on the session, and faint
    spill reaches 2–50 px into them (boxes found: x = 30–439);
  - cropping moves the block grid, and that alone moves single frames by
    ±1 dB all over the picture;
  - on a 16:9 screen the cut limit is the stricter one today (no bar cells
    in the count): the fight frame above would be repeated there. Not
    changed; it wants 16:9 captures of fights and of real cuts.

### 4.7 A HUD over fast scenery (11-15/16)

**Status:** on by default since 11-16 (4.8): `StillHud` (the evaluator's
default too, `--set hud_detail 0` is without it; when this was written it
was off and `--set hud_detail 0.04` turned it on). With it off the frames
are as before and WarpPS compiles to the same instructions. First run with
the card on 11-16 (4.8).

It is what 4.6 lost with the bars, and what a 16:9 screen never had: icons
and digits over the scenery in a fast turn came out faded, in pieces or
doubled.

- **What goes wrong** (the cave game, `debug_view` 1, 4 and 10): a HUD's
  cells take the scenery's motion - most of their pixels are scenery. Then
  - the rule that keeps what both frames show (WarpPS's persist) is off: it
    holds only near a cell that stands still;
  - the scenery is on offer from every cell around, also drawn from one
    frame alone at its fixed cost: it shows through an opaque icon;
  - a's and b's blocks there move too, so a one-frame option may take the
    icon's own pixels along: a faint copy of a digit half a step (70-110
    px) from the real one.
  Beside a bar none of this happened: the coarse blocks, held still by the
  bar, made every cell near it stand still - the scenery's as well, wrongly.
  With persist allowed everywhere (a quick test, not kept) the icons came
  back and the ten captures of 11-15 lost 0.18 dB: it has to be told where.
- **Static detail** (`StaticDetail` in MidCS, per cell that moves): the
  pixels with an edge (luma gradient 0.04 and up in a and in b) that is the
  same edge in both frames where it is - as bright within a quarter of the
  contrast, facing the same way within half of it. Three kinds of scenery
  pass that, and each was found the hard way:
  - *stairs in a turn:* a step's line crosses its own moved copy at a
    shallow angle and is the same edge there in both frames. The stairs
    came out in patches (20261029-185347-372: -0.61 dB). So the edge must
    lie across the cell's motion (`hud_across` 0.5); one along it says
    nothing.
  - *pillars in a pan of 130 px a frame and more:* one pillar's edge lands
    where another's was, and the motion found is a few pixels off, so "the
    same again where the motion takes it" fails by a hair. A patch of
    pillar stood still (two confident losses in the first blind review).
    The test now looks 1 px + 3 % of the motion's length further and nearer
    along it (`hud_slack`).
  - *a cart's leg, the same way:* there the cell's own motion was the
    wrong one of the two edges while a's and b's blocks had the pan. Their
    motions explain an edge as well.
- **Two designs.**
  1. *In WarpPS* (dropped): cells with static detail count for `near_still`,
     near them the one-frame options pay for bringing something else, and
     `StandsStill` is asked there whatever the option costs. It worked
     (HUD error as below, captures +0.01 dB). But the evaluator's warp stage
     went from 3.16 to 4.59 ms **with the rule off** - 3.45 after the first
     addition (nine texture loads in a branch never taken, one debug view),
     4.17 after two more arrays of ten floats. `[branch]` on everything
     changed nothing. WarpPS is at a size where anything added to it costs,
     taken or not.
  2. *HoldCS* (kept): a cell with 6 such pixels (of 64), and the cells
     right around it, get no motion - at the frame in between and as a's and
     b's blocks (the three grids coincide at level 0). WarpPS is untouched
     and treats the HUD as it treats anything that stands still. Cost: not
     measurable (4.14-4.28 ms a frame before, 4.16-4.28 with it on; the
     "motion at t" stage 0.40 → 0.48 ms).
- **Numbers** (the final code; "switches" = `PictureOnly` + `EdgeRules`):

| | switches | + StillHud | whole frame | + StillHud |
|---|---|---|---|---|
| clips, mean | 44.11 dB | 44.08 (`hud` -0.30, `noisy_hud` -0.11, `subs` -0.07, `orbit` -0.05) | 42.93 | 42.90 (the same four) |
| captures, skip-one (32) | 31.05 dB | 31.06 (2 better, 0 worse) | 30.35 | 30.36 (2 better, 0 worse) |
| the owner's ten of 11-15 | 30.57 dB | 30.60 (3 better, 0 worse) | 30.16 | 30.17 (1 better, 0 worse) |
| ... with `HistorySearch` | 30.58 dB | 30.61 (3 better, 0 worse) | | |

  HUD error (`eval\hud_score.py`: levels of 255 off the pixels that are the
  same in all five real frames, on the four adjacent pairs):

| capture | whole frame | switches | + StillHud | whole frame + StillHud |
|---|---|---|---|---|
| cave game, 20261115-021952-376 | 1.95 | 7.64 | 1.95 | 1.93 |
| 021959-650 | 2.83 | 24.14 | 0.88 | 1.09 |
| 022021-405 | 2.44 | 6.02 | 0.98 | 1.10 |
| 022029-781 | 0.77 | 0.79 | 0.79 | 0.76 |
| 022041-127 | 1.11 | 1.25 | 1.24 | 0.96 |
| 214452-990 | 4.99 | 6.80 | 2.33 | 2.76 |
| 214603-809 | 1.43 | 1.34 | 1.15 | 1.05 |
| 214613-851 | 1.65 | 21.92 | 1.14 | 1.25 |
| another game's digits, 20261031-144655-466 | 5.87 | 3.37 | 1.69 | 2.25 |

  The other 33 captures have a HUD with opaque panels of its own: 0.34 to
  0.99, every version within 0.06.
- **Blind reviews** of the adjacent pairs (a fresh Claude agent each time,
  the regions that differ most, at most two per capture, which is which
  randomised per region), switches against switches + StillHud:
  - the hold without the last two fixes: new 13, old 6, same 4 (confident
    6 / 2). All 12 HUD regions went to the new one; the two confident
    losses were the pillars above.
  - with the pillar fix: new 13, old 2, same 5 (confident 8 / 1). The
    confident loss was the cart's leg; after the fix for it those frames
    are the ones the switches alone draw. Not reviewed again after it.
- **What it costs:** the scenery in a held cell stands still with the HUD -
  a cell (16 px) around a panel's edge, a double image there. That is the
  `hud` clip's -0.30 dB (a cloak passing the panel). Narrower holds were
  tried on the ten captures, each kept more of the clip and less of the
  HUD (mean HUD error, `hud` clip): no cells around 0.83, -0.10; the cells
  around only as a's and b's blocks 0.77, -0.20; only cells around that
  are themselves mostly unchanged 0.81, -0.10; that with half the
  threshold and a looser ring 0.65, -0.85; all cells around (kept) 0.61,
  -0.39 before the three fixes above. What protects a small icon is that
  the scenery cells around it stand still, not its own inside.
- **Checked in the add-on** (no card; a copy of the app folder):
  - five frames of a cave capture played in a loop (1280x720 of it, SDR):
    with `StillHud=0` the add-on's frames are the evaluator's with the rule
    off on 95-98 % of the pixels the rule changes (3 % with it on), with
    `StillHud=1` the evaluator's with it on on 95-100 % (2 % off). HUD error
    in the add-on's own frames 17.7 → 3.6.
  - the checkbox on ReShade's Add-ons page (posted keys): on and off, the
    log's "StillHud=1 (from the panel)", the value in ReShade.ini.
  - a scrolling picture with boxes drawn over it says nothing: the search
    holds such a HUD by itself, and the rule changes no pixel.
  - `tools/smoke_raw.py`: HDR passed twice. SDR is marginal in its five
    seconds that night with either add-on (the picture stands for the first
    five while DLSS 5 starts): yesterday's passed 3 of 4, this one 2 of 4,
    and with 12 s it passes.
- **Leftovers:**
  - faint copies of a see-through icon or a digit beside the HUD in the
    fastest turns (fewer than before: 76 → 15 pixels in the worst pair
    under the first design; the hold was not counted);
  - at 130 px a frame and more near the picture's edge, with `PictureOnly`,
    a dark block of scenery beside the icons (a motion of -214 px found,
    both samples dark). The scenery's own matter; the bars hid it by
    holding everything there still.
- **Not known:** SDR games (the thresholds are on encoded luma, PQ here);
  HUDs of other games - large see-through panels, a HUD that itself moves
  or pulses. In play: 4.8.
- **Also learned:**
  - `fg_eval` is not always the same twice: on a few frames (9 of 504 in
    one run of everything) small patches on the main character differ
    between runs of the same build, the one from before this round
    included. It is in the layer passes; not looked into. Compare more than
    one run before calling a frame changed.
  - Time the stage against the build before, with the new thing off:
    `fg_eval --profile`, the builds in turn, three rounds. A single run
    moves by 1 ms with what else the GPU does.

### 4.8 The picture alone, the edge rules and the HUD hold by default (11-16)

`PictureOnly`, `EdgeRules` and `StillHud` are on unless `ReShade.ini` says
otherwise. `HistorySearch` stays off.

- **Why now:** the owner played with all four on (11-16; 3440x1440 HDR,
  DLSS 5 on, the picture 2560x1440 at 440,0 by the log): "looks good in
  use". It is the first time `StillHud` ran with the card.
- **Offline, whole frame → the three** (4.6 and 4.7 have the steps): the 32
  captures 30.35 → 31.06 dB skip-one (28 better, 2 worse), the owner's ten
  of 11-15 30.16 → 30.60 (8 better, 2 worse), the clips 42.93 → 44.08 (no
  bars in them: the edge rules and the hold). The HUD error of 4.7 is at or
  under the whole frame's but on two captures (+0.02 and +0.13 of 255).
- **A set:** `PictureOnly` without `StillHud` has 4.6's loss (the HUD near
  the picture's edge), so neither is a default without the other.
- **`Quality=fast`** (`warp_ring` 2, `warp_taps` 1, `soft_temp` 0.02) was
  measured for this, as nothing before had been:

| fast | whole frame | PictureOnly + EdgeRules | + StillHud |
|---|---|---|---|
| clips, mean | 41.19 dB | 42.67 | 42.65 (`hud` -0.26) |
| captures, skip-one (32) | 30.20 dB | 30.89 (28 better, 3 worse) | 30.90 (2 better, 1 worse by 0.02; from the whole frame 29 / 2) |
| the owner's ten | 30.00 dB | 30.40 | 30.44 (3 better, 0 worse; from the whole frame 8 / 2) |

  The HUD error with the three is at or under the whole frame's on the cave
  captures but for one (1.74 against 1.70). The hold is no slower in fast
  mode (a frame 7.1-8.0 ms with it, 7.4-9.1 without, the GPU busy with
  something else that evening).
- **`HistorySearch` not:** the owner has it on, but what was measured on
  the games played since says it does little there: +0.005 dB on the ten
  with the three on (30.602 → 30.607), nothing clearly better in 4.5's
  review of the cave game. Its gains are 4.5's, on the prototype's captures.
- **How a default is kept.** The add-on writes every setting's default into
  `ReShade.ini` at its first start and the file rules from then on, so a
  default that has shipped once cannot be changed for anyone who ran it.
  The four switches are kept another way (`switch_setting` in
  `framegen_addon.cpp`):
  - a missing key is the default, and stays missing;
  - a key that says what the default says is taken out, at start and when
    the panel puts a switch back (ReShade removes a key given a null value,
    6.4 on; the add-on is built for 6.8's interface and loads in nothing
    older);
  - so the file holds a switch only while someone chose against the
    default, and a later version can change it for everyone else;
  - `ReShade.log` says `switches: PictureOnly=1 EdgeRules=1 StillHud=1
    HistorySearch=0` at start, as the file no longer does (bug reports).
  None of the four was in a release (0.2.0 has none), so nobody's file
  holds an old default.
- **The evaluator follows:** `fg::Interpolator` comes with the edge rules
  and the hold, so `fg_eval` without `--set` scores what ships (with
  `--picture` on captures with bars). As before them: `--set edge_cost -1
  --set edge_explains 1 --set hud_detail 0`. Checked: without flags the
  clips and the 32 captures score line for line what 4.7's flags did (44.08,
  31.06 dB), with the three off what the whole frame did (42.93, 30.35).
- **Checked in the add-on** (no card; a copy of the app folder, the 960x720
  pattern with boxes in a 1280x720 window, SDR):
  - the owner's file (all four at 1): the three keys are gone after the
    run, `HistorySearch=1` stays;
  - no switch in the file (someone coming from 0.2.0): the three on
    ("PictureOnly=1: the picture is 960x720 at 160,0", "StillHud=1: ..."),
    30 real + 29-30 generated fps, the file still without them;
  - `PictureOnly=0` and `StillHud=off` are used and left as written,
    `EdgeRules=maybe` is warned about and left, `HistorySearch=0` is taken
    out;
  - the panel (posted keys): Still HUD, Picture only and History search
    each off and on - away from the default the key is written, back at it
    the key is gone and the log says "(from the panel; the default)";
    "PictureOnly=0: the whole 1280x720 back buffer again" and back.
  - ctest 4 of 4. `tools/smoke_raw.py --format p010 --hdr` failed its first
    three runs on that copy and then passed 4 of 4, and 3 of 3 with the
    three switches off: the picture stands for the first five seconds while
    DLSS 5 starts ("mpv fell behind: 150 of 165 new frames dropped"), with
    the switches on or off, and the test measures seconds 2 to 5.
- **Not known:** SDR games and a 16:9 screen in play (the picture alone
  *is* a 16:9 frame, so the offline numbers cover what is drawn there);
  other games' HUDs (4.7 found three kinds of false HUD in one evening on
  three games); other GPU makes.
- **Also learned:** a Space posted to ReShade's overlay is lost now and
  then when the real mouse has moved since the last arrow key (the owner at
  the PC; ImGui seems to leave keyboard navigation). Post an arrow key
  first, and read the log for what was switched.

## 5. The monocular-depth experiment

From 10-29 to 10-31. The question: can a depth estimate per frame tell the
character from the floor and from NPCs, where the shader's colour matching
cannot? It was done in separate folders (`FrameGen-Depth`, later the app copy
`MPV-SW-Capture-FrameGen-Depth`); the shipped add-on was never touched.

### 5.1 Setup

- **Model:** Depth Anything V2 Small (Apache-2.0; the Base model is
  CC-BY-NC). Input is the PQ frame turned into SDR; output is 16-bit, larger =
  closer, normalised per frame between percentiles 1 and 99.
- **Shaders:** depth maps bound to every pass (t14/t15), with `depth_mode`
  and parameters in the constant buffer, `fg_eval --depth DIR`, and
  `debug_view 11`.
- **Only frame A's depth is needed.** It can be computed one frame ahead, so
  it adds no delay.
- **Speed** (fp16, ms per map; the 5070 Ti was busy with the game):

| engine | 378x672, 4070 SUPER | 518x924, 4070 SUPER | 518x924, 5070 Ti |
|---|---|---|---|
| TensorRT 11.4 | 2.5 | 4.8–5.0 | 3.0–3.5 |
| TensorRT-RTX 1.7.1 | 2.6–2.7 | 4.3–4.4 | 3.1–3.4 |
| DirectML (ONNX Runtime) | ~8 (incl. copies) | ~29 | 19.5–21 |

- **What the numbers taught:**
  - DirectML needs fixed input sizes, or shape ops fall back to the CPU
    (240 ms);
  - DirectML is ~3x slower than TensorRT;
  - fp32 is 3–5x slower than fp16;
  - TensorRT engine builds take 1–2 minutes per GPU and size, giving 60–70 MB
    plan files.

### 5.2 Rounds

- **Round 1:**
  - `depthmask`: a small gain, +0.01 ms.
  - `depthgate`: needed frame B's depth; about base; dropped.
  - `zorder` (nearer wins in WarpPS): fixed the floor showing through NPCs,
    head +0.4 dB, but +1.4–2.2 ms with hard edges and blocks.
- **Round 2:**
  - `depthselect`: depth picks the player over NPCs and capes, and builds the
    layer from depth for dodges. Frame A only, +0.1 ms; only when the dominant
    motion is 24 px or more.
    - hole score 4930 → 3372; NPC confusion fixed.
    - Review verdict: ship with fixes.
  - `zorder2`: softer, half-resolution, +0.55 ms, NPCs drawn solid.
    - Review: don't ship (false dark hair tips, holes in spears and capes,
      sky speckle).
- **Round 3:** `depthselect2`, on top of the shipped non-depth "pick3".
  - It changed only 2 of 152 frames: hole 4558 → 3696, head
    27.899 → 27.974.
  - All 15 review findings were fixed.

**Conclusion then:** depth works as a signal, but on top of the non-depth
fixes it only rescues rare dodge and spin frames.

### 5.3 The live depth app (the owner asked to try it)

- **Pipeline:**
  - after DLSS 5, the add-on copies frame A to a D3D12 readback worker;
  - shared memory (`Local\msc_fg_depth_<pid>`, 3 slots, fp16 924x518 in,
    float32 out) feeds a Python TensorRT helper on the RTX 4070 SUPER (the
    second GPU);
  - the map comes back for the next pair;
  - F7 switches it.
- **Costs:**
  - ~0.2 ms on mpv's thread;
  - ~0.2 ms of 5070 Ti time per generated frame;
  - the helper ~6 ms on the 4070;
  - no pacing cost.
- **Coverage:**
  - at 30 fps, depth arrived for 100% of pairs;
  - at 60 fps with neural rendering, almost never: the map arrives 20–24 ms
    after the frame, more than the 16.7 ms interval. Frame generation backs
    off there anyway.
- **Single GPU (5070 Ti only):**
  - 30 fps works: depth on 95–99% of pairs, 0–2 late frames per 5 s;
  - 60 fps degrades: depth on ~35%, ~22 late frames per 5 s.
- **Hardening:** code reviews found fence, stall and shutdown bugs in the
  link, and helper bugs (heartbeat while loading, log spam, event-name
  squatting). All were fixed and tested with forced failures (stalls, a
  watchdog that terminates a hung helper, retry with F7).
- **Quality on 40 captures:**
  - depth changed 8 of 160 adjacent frames and 17 of 80 skip-one frames;
  - error against the real frame favoured depth on 11 of 17;
  - but the heads it keeps had holes, flecks and square edges, and the
    visual judges disagreed.

**Outcome (10-31): shelved.** After playing with it, the owner judged it "not
bad but not a major step up" and stayed with the main implementation. The
folders are kept.

**Why depth was not worth it here:**

- it needs a second GPU or costs too much at 60 fps;
- it needs ~400 MB of TensorRT DLLs kept out of mpv, a helper process and
  cross-GPU copies;
- the non-depth character layer already fixes most cases.

**Revisit only if new captures show failures the main path cannot fix.** The
z-order idea (NPCs solid) is the other lead, if its hair and weapon artefacts
can be avoided.

## 6. Pacing, VRR and latency

- **The hold** (GPU-side, 10-30):
  - the real frame is presented at once, behind a GPU wait on a D3D12 fence
    shared into D3D11;
  - a helper thread releases it half an interval after a D3D11 fence says the
    frame in between is done;
  - it is let go at the latest 0.5 ms before the next frame's effects, so
    the next DLSS 5 pass is never held.
- **Why not sleep on mpv's thread:** mpv needs ~3 ms from arrival to present,
  so a sleep-based version could not work at 60 fps.
- **Results with VRR** (PresentMon, on-screen intervals):
  - 30 → 60: 16.67 ± 0.4 ms / 16.67 ± 1.0 ms;
  - 60 → 120: 8.34 ± 0.3 / 8.33 ± 0.5;
  - no drops.
  - The old timing (real frame half an interval after mpv's present) gave
    uneven 15.0/18.3 and 5.2/11.5 ms.
- **Latency, mpv present → screen, VRR:**

| | 30 fps | 60 fps |
|---|---|---|
| frame generation off | 6.7 ms | 5.7 ms |
| generated frame | 8.5 ms | 7.5 ms |
| real frame | 25.2 ms (+18.5) | 15.9 ms (+10.2) |

  The real frame's extra is half a source frame plus drawing. The ~7 ms from
  present to screen for the frame in between is the DLSS 5 pass, not the
  interpolation: Fast quality saves only ~0.3 ms of it.
- **Repeats when frame generation is off** ("all frames when off"): 5.7 ms
  instead of 6.8 ms at 30 fps (the GPU runs each frame ~1 ms faster at 60
  presents/s). It is the low-latency option for people who don't use frame
  generation.
- **VRR only engages for the focused window.** NVIDIA's windowed G-SYNC
  follows focus (confirmed by the owner with the G-SYNC indicator). Tests
  must click the test window or run while the owner is away; check focus.
  The screen is ~165 Hz.
- **Window layouts (PresentMon, 11-01):**
  - borderless full screen, and a boxed top-level window (Lerad's full
    screen on an ultrawide, `src/app/backdrop.cpp`), both kept independent
    flip and VRR - the boxed one only while Windows gave it an overlay
    plane: after a reboot the same test (plain mpv, `--no-config`) was
    "Composed: Flip" boxed, independent flip only at exactly the monitor's
    size (3440x1439 was composed too). Composed, presents wait a frame in
    DWM, the queue fills and frame generation gave ~40 of 60 generated
    frames ("queue full") with the GPU 19 % busy. Cause not found (not the
    second screen, taskbars, SDR/HDR, Wallpaper Engine); the boxed window
    became an option, off by default (`window.skip_black_bars`, 11-01);
  - an mpv child window (`--wid`) loses VRR.
- **NVIDIA "Prefer maximum performance" for mpv** made 60 fps work ~2 ms
  slower per frame. Leave power at the default.

## 7. Dedup and the PS5's noise

- Frame generation needs new frames only: a 30 fps game captured at 60 must
  arrive at 30.
- The PS5's signal is not bit-exact on repeats. Each 60 Hz refresh adds
  zero-mean noise:
  - NV12: ~22% of pixels ±1–3 steps;
  - P010: ~63% ±1–6.
  - A per-sample threshold let every frame through.
- **Fix:** a change is a 16x16 block whose signed mean moved more than the
  threshold, or one sample more than 8x it. A frame is new from 4 changes;
  the threshold is 3.
  - PS5 noise peaks at 1.5–1.9 per block (NV12), ~0.6–1.0 (P010);
  - new frames show ≥ ~2000 changes.
  - Live: 303 new of 603 at 30 fps.
- The Switch 2 was bit-exact.
- **A capture at the game's rate needs no dedup (11-16):** an Elgato 4K X at
  3840x2160 HDR, 30 fps, from a PS5 Pro measured 2.2-3.0 a block (samples up
  to 24) against the threshold of 3. In the console's menus (a still
  picture) each frame was new or a repeat by chance: "12-25 new of 30.5
  captured", a source of "14-30 fps, variable", x4 swinging between 60 and
  120 fps. In motion it read 30.5 of 30.5.
  - Frame generators inside a game (DLSS, FSR) pace by its presents and
    never compare pictures; a still pair comes out as the same picture.
  - `frames.every_frame = auto` does the same when the capture runs at 30
    fps or less: no filter, every frame stamped as new. A game stutter then
    shows as a still pair, as it would on a TV.
  - Not yet run on the card.
- **A faster capture holds the game's rhythm (11-17):** a PS5 Pro at
  3840x2160 HDR 59.94 through a Magewell Pro Capture HDMI 4K Plus read
  "11-48 new frames/s of 60.4 captured", a source of "8-60 fps, variable",
  the frames shown swinging between 60 and the low teens.
  - Measured against the frame before (59.94, 16x16 blocks, threshold 3):
    repeats and a still picture 0 changes (blocks move 1.4-2.1); a moving
    picture 20-190; a slow fade's real frames 2-9, on both sides of the
    line of 4. The noise is the PS5's, not the card's.
  - `CadenceHold` (repeat_filter.h) reads the rhythm from the frames that
    leave no doubt (16 changes or more, or under 4) over the last 16
    frames: every frame new, or every other one. It is then kept through
    pictures that hardly move; a moving frame out of turn takes the turn.
    An uneven rate is compared frame by frame as before.
  - On with `frames.every_frame = auto` (off: compare only). Unit tested;
    not yet run on the card.

## 8. The GPU budget

- At 60 → 120 each frame has ~8.3 ms for everything. DLSS 5 neural
  rendering costs:
  - ~9.7 ms at 3440x1440;
  - ~5.5 ms on the boxed 2560x1440 picture;
  - ~15.7 ms per frame in early tests with neural rendering on.
- **Back-off:** when mpv drops frames, frame generation pauses for 5 s,
  doubling up to 60 s (reset after 30 s clean). The menu says so ("Paused,
  back in N s"), and also when the display cannot show twice the rate.
- **Fast quality** roughly halves the interpolation's own GPU time
  (README), but the interpolation is not the big cost next to DLSS 5.
- **Boxed full screen** (11-01) was the biggest single gain: DLSS 5 and frame
  generation stopped working on the ultrawide's black bars (9.7 → 5.5 ms).
  An option, off by default, since the same day (`window.skip_black_bars`):
  it depends on an overlay plane Windows does not always give (section
  above), and without one frame generation loses about a sixth of its frames
  even with DLSS 5 off. Neither the DLSS 5 add-on
  (`NRResolutionScale` only scales the whole frame) nor the feeder
  (`work_resolution`) can be limited to the picture.
- GPU timings vary ~1 ms between sessions (clocks, background apps such as
  NVIDIA Broadcast or Wallpaper Engine). Compare A/B within one session.

## 9. Testing: tools and gotchas

- **Without the card:**
  - ffmpeg test pattern → `Lerad.exe --input-raw -` (`tools/smoke_raw.py`);
  - `LERAD_FRAMEGEN_CAPTURE_AFTER=<s>` takes an F9 capture in a test;
  - `LERAD_FRAMEGEN_TRACE` writes per-frame timing.
- **Pacing tests:**
  - ffmpeg `-re` jitters ±12 ms and VRR shows it directly; a steady pacer
    (`raw_pace`) fixed that;
  - use a mean, not a median, for ffmpeg's source rate.
- **Warm-up:** the first ~5 s always drop frames (DLSS 5 starting); compare
  against a frame-generation-off baseline before blaming frame generation.
- **Reading PresentMon:** VRR runs show SyncInterval 0 / AllowsTearing 1 with
  intervals off the refresh grid; fixed-refresh runs show 1/0 and on-grid
  times.
- **The owner's PC:**
  - test pipelines rewrite ReShade.ini (window size, font scale): copy it
    before and back after;
  - never run two pipeline tests at once (same pipe and link names);
  - a background process cannot take focus from the owner, and screen grabs
    show whatever they have open: capture the test window with `PrintWindow`
    instead;
  - mpv ignores keys posted from another process (ReShade's input sees them);
    use mpv's IPC `keypress`.
- **Captures:**
  - the 3440-wide captures are pillarboxed (picture at x = 440–3000), so
    "off screen" checks don't fire at the picture's edge (that alone costs
    nothing, section 4.6); score them with `--picture 440 0 3000 1440` to
    see what PictureOnly draws (the add-on's default since 4.8);
  - the owner prunes captures: never put deleted ones back.

## 10. Known leftovers and open ideas

- **Leftovers:**
  - a spike that turns while the head drifts comes out soft or cut;
  - a faint wisp beside a restored spike;
  - a blocky hair fringe in one orbit;
  - NPC fragments torn over hair when an NPC slides in behind the head;
  - a whole character smears when body and hair move differently (the
    dodge);
  - a HUD near the picture's edge over fast scenery: thin and see-through
    parts fade or double with `StillHud` off (section 4.6). On, as it comes
    (4.7, 4.8), it holds them at the price of the scenery right beside
    them; faint copies of see-through parts remain. The HUD mask (section
    14, off by default) puts the HUD's own pixels right and leaves the
    copies beside it;
  - small patches on the main character that differ between two runs of
    the same build on the same frames (4.7).
- **Ideas not pursued:**
  - finer motion at object edges (4x4 on boundary cells only);
  - x3/x4: built 11-12 as an option (section 11); its picture quality on
    game captures is not judged yet;
  - frame generation on the second GPU;
  - an offline VFG comparison;
  - a z-order rule that keeps NPCs solid without hair artefacts.

## 11. More than x2, screens without VRR, honest numbers (11-12)

Done in Lerad on a PC with an RTX 5090 and a 240 Hz screen, with a synthetic
source (a Python pacer writing NV12 into `Lerad.exe --input-raw -`); no
capture card, no game captures, no focused VRR window. Tools: the add-on's
trace with its new `#screen` lines, and PresentMon 2.6 (it runs without
administrator rights for members of "Performance Log Users"). The numbers
are in [src/framegen/README.md](../src/framegen/README.md), "What the screen
does to the pacing".

### 11.1 x3 and x4

- **What it took:**
  - the shaders already take any position between the two frames
    (`DrawOptions::t`), so no shader or `fg_interp.h` change;
  - the first frame in between is drawn in `reshade_present` as before, the
    later ones and the real frame in `finish_present`, each behind its own
    GPU wait; the hold's thread lets them go a step apart, timed from when
    the first was done;
  - `MultiplierGate` picks the most frames that fit the screen, up to the
    setting.
- **Proven:** the steps on the CPU (x3 11.11 ms sd 0.01-0.05, x4 8.32-8.34 ms
  sd 0.13 at 30 fps), and where the frames sit (0.24 / 0.50 / 0.76 and
  0.34 / 0.66 of a moving bar's step, from F9 captures).
- **Not proven:** the on-screen spacing with VRR engaged (it follows from
  the x2 measurements of section 6 only if the screen behaves the same for
  shorter steps), the GPU budget next to DLSS 5 at 4K, and the picture
  quality of thirds and quarters on game captures.
- **A bug found by measuring, not by looking:** the later frames were first
  drawn at (n - 1) / count instead of n / count, so x4 showed the first frame
  in between twice. The frames looked fine one by one; the bar's position in
  the captures gave it away. Check positions whenever t is touched.
- **Cost:** every frame in between runs the whole interpolation; mpv's
  thread is busy until the second to last frame is let go (13.5 ms at
  30 fps x4). At 60 fps x4 the steps were squeezed (3.83 instead of 4.17 ms)
  to be out before the next frame's effects.
- **Latency:** the real frame is shown (count - 1) / count of a source frame
  after the first frame in between: 16.7, 22.2 and 25 ms at 30 fps for x2,
  x3, x4.

### 11.2 Dead ends

- **Sync intervals instead of the hold on a fixed-refresh screen**
  (`Present(n, 0)`, n = the refreshes the frame should come after the one
  before): in a composed window 30 → 60 on 240 Hz gave 2 and 6 refreshes,
  not 4 and 4; x4 gave 3, 2, 2, 1. A present with n > 1 also counts as queued
  for the "queue full" rule, which then left out most frames. Removed.
- **A lead found by feedback** (let held frames go earlier until the frame
  after the first one lands the wanted number of refreshes later, read from
  the swap chain's frame statistics):
  - reading the statistics on the hold's thread deadlocked with mpv's blocked
    present at x3 and x4 (one-second stalls);
  - in a composed window the reading is late and mixed with "queue full"
    effects: at 60 fps the lead ran to its limit and made things worse.
  Removed. What stayed is the fixed test switch `LERAD_FRAMEGEN_LEAD_MS`.
- **Taking the focus for a measurement:** a window started from a terminal
  the user is typing in does get the focus. The test runner now calls
  `LockSetForegroundWindow` first and gives the focus back if it moves.

### 11.3 Screens without VRR (or with VRR not engaging)

- The hold times the real frame from when the frame in between was *done*.
  On a fixed refresh "done" comes just after a refresh (a constant 4.55 ms
  after the add-on drew, in a composed window on 240 Hz), so a whole
  number of refreshes later is just too late for the refresh meant: 5 and 3
  refreshes instead of 4 and 4 at 30 → 60 on 240 Hz.
- Letting go 5 ms earlier fixed most of it (78 % of frames on 4 refreshes);
  2 ms did not (a third each on 3, 4 and 5). The lead needed is about a
  refresh plus Windows' composition time there, and will be less for a
  window shown directly (not measured: it needs a focused full-screen window
  on a fixed-refresh screen).
- **The plan, if this is taken further:** a sampler thread of its own (like
  the trace's, never the hold's), only while the screen is seen refreshing
  at a fixed rate, that reads on which refresh each present was shown; from
  that, move the lead a quarter refresh at a time until the frame after the
  first lands the wanted number of refreshes later; stop adjusting while
  "queue full" is counting. Test on a focused full-screen window with VRR off
  in the driver, then with VRR on to see that nothing moves.
- **Telling that VRR is not engaging:** the swap chain's `SyncRefreshCount`
  read once a second gave 240.0 refreshes a second in every run here
  (composed, fixed). The add-on reports that when clearly fewer frames are
  shown. Still to confirm on a focused VRR window: that the count then
  follows the frames (if it does not, the warning would be shown wrongly
  there; it changes no pacing).

- **A fixed 120 Hz screen, measured (11-12, later the same day):** the
  second screen of the same PC, 3840x2160 at 120 Hz with VRR not offered by
  the driver, a synthetic 3840x2160 P010 source at 30 fps, borderless, not
  focused, composed (PresentMon: "Composed: Flip", sync interval 1).
  - 30 → 60: 2 and 2 refreshes for 88-94 % of the pairs (95 % of frames on
    2 refreshes), 57-58 frames shown a second; the rest are frames left out
    as "queue full" (a 4 and 4 pair) and a few 3 and 2. The 5 and 3 pattern
    of the 240 Hz screen does not show: a refresh is 8.3 ms here, and being
    a few milliseconds late still lands on the refresh meant.
  - 30 → 120 (x4): one refresh for 90 % of frames, about 103 of 120 shown a
    second. Every frame needs its own refresh, so each one left out as
    "queue full" (12-54 in 5 s) is a doubled refresh. No lead would help:
    there is no spare refresh to move into.
  - x3 asked for: the gate stays at x2 (90 fps does not divide 120 Hz).
  - So on a fixed 120 Hz screen the flaw above does not matter for
    30 → 60; it is specific to screens where a frame should stay four or
    more refreshes.
- **`SyncRefreshCount` counts the wrong screen there:** on that 120 Hz
  screen the add-on's log read "seen refreshing at 240.0", the rate of the
  PC's other (main, 240 Hz) screen. For a composed window the count
  follows Windows' compositor, not the screen the window is on.
  `judge_refresh` calls 240 on a 120 Hz screen Unknown, so nothing wrong is
  shown, but "VRR is not engaging" can then never be shown on a screen
  slower than the fastest one.
- **Fixed 11-18: the screen's own count.** A thread of the add-on waits
  for each vertical blank of the screen the window is on
  (`D3DKMTWaitForVerticalBlankEvent` on that screen's video source, opened
  by Windows' name for it; `src/framegen/fg_vblank.h`), and the count is
  read once a second.
  - Why this call: it wakes with DXGI's `IDXGIOutput::WaitForVBlank`, the
    same gaps to the microsecond
    (Chromium and Qt follow a screen's refreshes with that on a thread; the
    wait sleeps, 0.14 s of processor time in 12 s), and opened by name it
    needs no DXGI output, which goes stale when screens change.
    `IDXGIOutput::GetFrameStatistics`, thought of first, was not tried.
    Windows' compositor's own counter (`DwmGetCompositionTimingInfo`) read
    0.2 a second while mpv was flipped straight to the screen: no use.
  - Measured on the one screen switched on (120.000 Hz, with a small probe kept outside the repo):
    120.00 a second by this call and by `WaitForVBlank` alike, no refresh
    missed in 600, at normal thread priority too. While Lerad showed
    59.94 x2 with VRR on, both read 119.86-119.90 with gaps of 8.2-10.1 ms:
    the refreshes followed the frames.
  - The add-on now looks up the window's screen every second
    (`MonitorFromWindow`, not the swap chain's `GetContainingOutput`) and
    takes that screen's rate and VRR state (Lerad.exe hands over every
    screen's at start, `LERAD_SCREENS`); count and judgement start over on
    a change, and the log says so.
  - Not seen: two screens at once, the window moved between them, a screen
    switched off while counted, and a VRR screen well below its top rate.

### 11.4 The numbers shown

- The menu's old line came from the 5 s log window and showed twice the
  source rate while frames were being left out. Now the rates are counted
  over the last second from the presents themselves, the source's from the
  link's arrival times, and the reason is the latest frame's decision (or,
  for the ones that come and go, what was counted in the last second).
- While frame generation is off mpv gets the repeats too, so "frames
  presented" is the capture rate; the menu then gives the game's rate only.

### 11.5 A source whose rate moves

- Fed 40-58 fps swinging every 4 s on a 120 Hz grid: no "too fast", no
  back-off, no resets; the interval (a mean of the last 14 gaps) followed
  between 17.9 and 25 ms, and the log and menu read "40-57 fps, variable".
- The real frame is held half the *mean* gap, so with gaps of 16.7 and 25 ms
  the two halves are uneven by a few milliseconds either way. The next gap
  cannot be known; nothing better was found, and nothing was changed.
- One thing was: a rate that wanders across what the screen can show (a VRR
  game at 80-90 fps on 165 Hz) switched generation on and off with every
  swing. `MultiplierGate` now gives what is running 3 % more room and waits
  two seconds before going up again.

### 11.6 A multiplier that fits and cannot be held: the step down (11-18)

Upstream (Abhito/Glasir 85a9253) steps 3x and 4x down where "the GPU cannot
hold" them: a counter that goes up 3 for each real frame left without its
frames ("queue full") and down 1 for each that got them, one fewer at 18,
tried again after 20 s and then twice as long each time (5 minutes at
most), and at once when DLSS 5 or a setting is switched. Its case is DLSS 5
finishing 23-27 ms into a 33 ms frame. Ours was written for this code
(`StepDown` in `fg_pacing.h`, tested in `tests/test_fg_pacing.cpp`; the
add-on only feeds it and logs); the waits are upstream's.

- **The case here** (RTX 5090, the 120.000 Hz screen, a 1280x720 window
  without the focus, so composed; a 1920x1080 game clip as a raw 30 fps
  source in a 60 fps stream through `raw_pace`, DLSS 5 off; the add-on's
  trace gives what each game frame got):
  - x4, as it was: every other game frame shown alone. Each second 15 game
    frames with four frames and 15 with one, 75 frames shown, 75 "queue
    full" in 5 s, for the whole run (two runs, 28 and 30 s). After the
    start the hold was not squeezed (0 "let go early", 0 "already due"; the
    first added frame done 4.6 ms after it was drawn), so it is not the
    GPU: four presents in 33.3 ms are all a 120 Hz screen takes, and a
    composed window has no way to make up for one that waits.
  - x3 in the same window: 90 frames a second, 0 "queue full" after the
    first seconds.
  - A 20 fps source at x4 (80 frames a second): the same, 0 in 35 s.
- **It differs from run to run.** Of 12 runs started at x4, 7 lost every
  other game frame from the first second. The other 5 lost a game frame's
  frames only now and then - one every 3 s in the two followed longest (117
  and 120 frames shown, 44 s) - and one of them more and more after 27 s.
  The source's 30.00 and the screen's 120.00 are two clocks; where the four
  presents fall between two refreshes is set at the start and drifts. So a
  test of this needs several runs, and a run that "holds" may not hold a
  minute later. (x3 lost one every 3 s too in one run, for 20 s; where that
  period comes from was not looked for. It is under the rule's limit: x3
  stayed.)
- **The rule and its numbers:**
  - Down at 4 game frames shown alone within 2 s, the first and last half a
    second apart. The measured failure is 15 a second: stepped down 0.57 s
    after frame generation started. One every 3 s (the other kind of run),
    one or two at a start, and a few in a row (a hiccup) stay under it: of
    the 5 runs that lost one now and then none was stepped down while it
    did only that.
    Whether one long frame every 3 s at 120 looks better than 90 even ones
    was not judged by eye; the limit could be lowered to catch it.
  - A try 20 s later; 2 game frames shown alone within 3 s of it end it. In
    the bad state that took 0.10 s every time (6 tries): two long frames,
    88 frames shown in that second instead of 90. Then 40 s, and so on to
    300 s.
  - At once, but not within 5 s of the step down, when the window, the
    screen, the picture's size, the source's rate or ReShade's effects
    change; everything forgotten when a frame generation setting is changed.
    Seen live: the window moved 40 pixels ("x4 is tried again (the window
    changed)", failed, 40 s), and F8 off and on ("no longer stepped down",
    x4 from the start, stepped down again after 0.57 s, 20 s).
  - mpv dropping frames at x3 or x4 steps down before the pause; the pause
    stays for x2. Not seen live.
- **Both generators:** DLSS Frame Generation in the same window: stepped
  down in the first second, x3 at 90 even frames, the try at 20 s failed
  the same way. With DLSS 5 engaged (ratio 0.998) and our generator: the
  same, x3 held for 50 s; the hold was not squeezed at 1280x720, so "the
  screen" was right there too.
- **StepDown=0** is the behaviour before, for comparisons (the panel's
  "Step down").
- **Not seen:** a window flipped straight to the screen or full screen
  (this PC is in use), VRR engaged, the capture card with a game in motion
  (the PS5's 29.97 x4 is 119.88, a tenth of a percent under the screen's
  120: the queue has room to drain there, which a 30.00 source does not
  give), the GPU as the cause (only the rule's tests and the menu's
  `MENU_DEMO=step-gpu`), 4K, HDR, and two steps down (x2) outside the tests.
- **Left as it was:** a setting changed during the pause ("Paused, back in
  N s") still waits the pause out; upstream tries at once there too.

## 12. Other frame generators

Researched on the web 2026-11-12; nothing here was run. Sources are the
vendors' pages and release notes where they exist, press and forum reports
otherwise.

- **NVIDIA Smooth Motion** (driver level; RTX 50, RTX 40 since driver 581.08
  of August 2025): DX11, DX12 and Vulkan, 64-bit, x2 only, set per program in
  the NVIDIA app or with Profile Inspector.
  - The one report with mpv
    ([mpv issue 17140](https://github.com/mpv-player/mpv/issues/17140)): it
    did nothing with mpv's D3D11 context, worked with `gpu-context=winvk`,
    warped subtitles, and later flickered after driver updates.
  - NVIDIA says its present layer can upset third-party overlays; with
    Special K injected one user's rate fell
    ([Special K issue 359](https://github.com/SpecialKO/SpecialK/issues/359)).
    Nothing found on ReShade as `dxgi.dll`, on HDR, or on windowed mode.
  - It would see Lerad's menu as picture, and a second generator on top of
    Lerad's adds latency for nothing (NVIDIA says the same of DLSS frame
    generation plus Smooth Motion).
- **AMD Fluid Motion Frames** (AFMF 2.1, Adrenalin 25.3.1; RX 6000 and
  newer): DX11, DX12, Vulkan, OpenGL; exclusive or borderless full screen,
  VSync off; switches itself off in fast motion; AMD's notes recommend a
  base of 55 fps at 1080p and 70 at 1440p, well above a 30 fps console game;
  its release notes list overlays as a reason for it to go inactive. Nothing
  found on mpv.
- **Intel:** no driver-level frame generation for programs that do not
  build it in (XeSS frame generation is for games that ship it).
- **Lossless Scaling** (LSFG 3, any GPU, paid): works on any window by
  capturing it, x2 to x20 or a target rate, G-SYNC support, a second-GPU
  mode. It counts the presents it sees, so Lerad's dedup (30 presents a
  second for a 30 fps game) is what it wants; it captures the finished
  window, menu included, and adds its own delay. HDR passthrough not
  confirmed.
- **Neural interpolation, again:** RIFE through VapourSynth and TensorRT
  runs 4K x2 in real time on an RTX 4090 or 5090 only with most of the GPU,
  as a filter that buffers frames; NVIDIA's Maxine Video Frame Generation is
  an SDK filter with no latency figures published. Both as in section 4.1:
  over budget for live play next to DLSS 5.
- **What Lerad should say:** its own frame generation for everyone; one
  generator at a time; Lossless Scaling for someone with a second GPU to
  give it; the driver options are not recommended until someone measures
  them on mpv's D3D11 swap chain with ReShade loaded.

## 13. The GPU makers' frame generation, run on video

Run 2026-11-18 on an RTX 5090 (driver 616.92). DLSS Frame Generation and FSR
frame generation are made for games: Direct3D 12, with depth, motion vectors
and a camera for every frame. Here they get mpv's Direct3D 11 pictures and
nothing else.

- **How they are reached** (`fg_bridge.h`): the add-on's two frame copies
  and the textures the made frames land in are Direct3D 11 textures with
  shared handles, opened on a Direct3D 12 device of the same GPU; a shared
  fence orders the two devices' work, so nothing is copied and the CPU does
  not wait. Depth is flat, the camera stands still, the motion vectors are
  zero. Presenting stays with mpv and the add-on, pacing and all.
- **DLSS Frame Generation** (`fg_dlss.h`, `framegen.backend = dlss`): works.
  - NGX is called in the driver's own library, with no NVIDIA SDK to build
    against. Its model (`nvngx_dlssg.dll`, 7 MB) has to be brought along:
    the driver has one, but gives it to no program that brings none.
  - It finds the motion itself. True vectors, zeros and vectors of the
    wrong sign gave the same frame.
  - Up to five frames between two on this GPU (Lerad stops at three, x4).
    About 1.1 ms a frame at 3840x2160, 1.7-1.9 at 5120x1440.
  - Every picture has to be given once per frame in between, also the first
    of a sequence; after a break the earlier picture is given again as a
    new start.
- **Against Lerad's own**, `fg_eval --methods repeat,blend,motion,dlss,fsr`
  on seven clips of 5120x1440 cut from 60 fps game recordings (three games;
  video-compressed, so the true frames carry that too):

  | | repeat | blend | Lerad | DLSS | FSR |
  |---|---|---|---|---|---|
  | mean PSNR, dB | 28.14 | 29.75 | 32.96 | 31.90 | 29.75 |
  | GPU ms a frame | - | 0.03 | 1.5-3.9 | 1.7-1.9 | 1.0 |

  - DLSS is ahead on two clips and close on two more; on fast pans it is
    3.5 dB behind: it smears the picture's edge, where Lerad's own shows
    blocks but keeps the shapes. On a HUD it is the cleaner one (digits stay
    whole where Lerad's ghost).
  - So it is a choice, not the default. Seven clips of three games is a
    small set; none was a console capture.
- **FSR frame generation** (`fg_fsr.h`, in `fg_eval` only): runs without its
  swap chain (`FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY`), and
  makes a plain mix of the two pictures: the same score as "blend" on every
  clip. With zero motion vectors it has nothing to move things by; its
  optical flow only corrects the game's vectors. It would need real vectors
  from an optical flow of ours, and then be no better than what that flow
  is. Not offered in Lerad.
- **XeSS frame generation** was not tried: its API is built around owning
  the swap chain.
- **Areas left alone** (`[framegen] keep`, `keep_regions.h`): in a frame in
  between, a rectangle or ellipse shows the nearer real frame as it is, for
  either generator. It keeps a HUD or subtitles sharp at the cost of their
  moving at the game's rate - and of everything else in the area doing so
  too (section 14).

## 14. The HUD found by itself (11-18)

**Status:** built, off by default. `[framegen] hud = 1` looks for the HUD
over the whole picture; an area left alone written with `,hud`
(`keep = r:2,80,20,15,hud`) only says where to look. `src/framegen/fg_hud.h`
and `hud.hlsl`, after either generator. Scored on SDR clips only; in the
add-on it ran on those clips fed as a raw source and on a console's still
home screen, not yet on a game in play.

- **Why.** A drawn area keeps the HUD exact, but the scenery seen beside and
  through it stands still as well, at the game's rate inside a picture that
  moves at twice that. On the seven clips below a hard box costs 1 dB of the
  whole frame and makes the scenery inside the boxes worse than the generator
  left it (11.15 levels against 8.74). `StillHud` (4.7) holds whole 16 px
  cells and only where the search already goes wrong.
- **The rule** (worked out first as a script on the same clips, then ported).
  Per pixel two small counters, kept from one pair of real frames to the next:
  - *still:* the pixel stayed as it was, nothing changing within 2 px of it,
    while a motion found for the scenery says it should have changed (a here
    and b where the motion takes it differ);
  - *edge:* an edge stayed the same edge - direction and strength, the level
    may change, as under a see-through HUD - across a motion that would have
    moved it.
  A pair with such evidence counts up; two pairs turn a counter on. A pixel
  that changes lets go of "still" at once, an edge that is gone loses "edge"
  in two pairs. The mask is where either is on, grown by 2 px. "A motion
  found for the scenery" is the interpolator's block motion from a to b, tried
  two ways: the pixel's own cell's, and the background's around it (per block
  of 8x8 cells the median of the cells that move), since a HUD panel's own
  cells stand still.
- **Applied** as one pass per frame in between: the nearer real frame where
  the mask is set, over the picture or inside the areas that ask for it.
- **The clips:** seven of 33 frames, 5120x1440, cut from 60 fps recordings of
  three games (video-compressed); pair k is frames 2k-1 and 2k+1, the true
  frame 2k. HUD boxes marked by hand per clip. Scored over pairs 5 to 16
  (`fg_eval --hud BOXES.json SEQ_DIR`): mean absolute error in 8-bit levels on
  the HUD's own pixels (inside a box, unchanged over the whole clip, with
  detail), on the rest of the boxes, and the whole frame's PSNR.

| own generator | as is | hard box | mask in the boxes | mask everywhere |
|---|---|---|---|---|
| HUD's own pixels, levels | 3.12 | 1.04 | 1.16 | 1.16 |
| rest of the boxes, levels | 8.74 | 11.15 | 8.63 | 8.63 |
| outside the boxes, levels | 9.71 | 9.71 | 9.71 | 9.72 |
| whole frame, PSNR dB | 26.37 | 25.41 | 26.36 | 26.36 |

| HUD's own pixels, per clip | as is | hard box | mask |
|---|---|---|---|
| bf6_a | 6.30 | 1.69 | 2.33 |
| bf6_b | 1.21 | 0.71 | 0.77 |
| bf6_c | 2.83 | 0.90 | 0.90 |
| ds2_a | 7.76 | 2.11 | 2.11 |
| ds2_b | 0.40 | 0.55 | 0.51 |
| ds2_c | 0.67 | 0.35 | 0.51 |
| wardogs | 2.64 | 0.99 | 0.97 |

  - The hard box's number is the floor: what the later real frame is off the
    true one by on pixels that "do not change" (video compression). On ds2_b,
    a menu, the generator's mix of two frames is under that floor, and both
    box and mask cost a little there.
  - The mask holds 89 to 98 % of the HUD's own pixels on five clips; 73 % and
    44 % on the two with a menu over a nearly still picture, where little
    moves to give evidence (and little is there to protect).
  - The script's numbers on the same frames: 1.17, 8.63, 26.36. The port
    differs in the motion it is given (the interpolator's own, HUD cells held
    still by `StillHud`, against a dense flow's block medians), in luma that
    is not rounded to whole levels, and in comparing a pixel's 3x3 under its
    own cell's motion only. Per clip the two are within 0.04 levels.
- **A cut is not told to it.** The first version cleared the counters when
  the interpolator took a pair for a cut. That signal fires on fast action,
  pair after pair, in three of the seven clips: HUD error 1.49 with the
  counters cleared, 1.23 with such a pair's evidence left out, 1.16 with the
  pair counted like any other (`cut_rule` 2, 1, 0). Across a real cut every
  pixel changes and is let go by the rule itself; what does not change there
  is a HUD that is still on.
- **With DLSS Frame Generation** there is no block motion to borrow. Two ways
  were measured on its frames:

| DLSS FG's frames | as is | hard box | mask, own motion search | mask, no motion |
|---|---|---|---|---|
| HUD's own pixels, levels | 4.39 | 1.04 | 1.14 | 1.18 |
| rest of the boxes, levels | 7.59 | 11.15 | 7.68 | 7.71 |
| whole frame, PSNR dB | 26.81 | 25.97 | 26.81 | 26.74 |
| GPU ms a real frame, 5120x1440 | - | - | 0.23-0.29 + 0.22-0.27 | 0.52-0.53 |

  - *Own motion search:* the interpolator's pyramids and its search one way,
    run for the mask alone (`fg::Interpolator::find_flow`).
  - *No motion:* detail that does not change while the frame as a whole does,
    counted over four pairs (the "static map" of TV logo detection). It
    covers nearly all of the HUD, and with it scenery that holds still while
    other things move (60 000 to 260 000 pixels outside the boxes on five
    clips, up to 0.3 dB off a clip), and needs two pairs longer.
  - The search is taken: the same result as with the own generator, the
    whole frame untouched, and no dearer. The rule without motion stays for a
    picture too small to search.
- **What it costs** (RTX 5090, `fg_eval --hud`, the median over a clip's
  pairs; 3840x2160 is the same clips scaled, for the time only):

| GPU ms | 5120x1440 | 3840x2160 |
|---|---|---|
| detector, a real frame (own generator) | 0.24-0.29 | 0.27-0.32 |
| ... with DLSS FG: the search, then the detector | 0.23-0.29 + 0.22-0.27 | 0.26-0.32 + 0.24-0.30 |
| the mask's pass, a frame in between, everywhere | 0.020-0.026 | 0.022-0.027 |
| ... inside the boxes only | 0.006-0.015 | 0.007-0.014 |

  The own generator's frame takes 1.3 to 3.9 ms there. With the mask off
  nothing runs and nothing is allocated; `fg_eval --methods motion` on the
  seven short clips of section 13 is 33.02 dB before and after, every saved
  frame the same file.
- **In the add-on** (a copy of the app folder, a 1280x720 window, nobody at
  the PC):
  - a HUD clip as a raw 30-in-60 source, SDR: of the pixels that stand still
    with detail in a capture's five real frames, the frames in between were
    the nearer real frame exactly on 13 to 67 % without the mask and on 93 to
    95 % with it - own generator and DLSS FG, x2 and x4, the switch and two
    areas (a rectangle and an ellipse). The mask held 6.5 to 9.5 % of that
    picture; the detector took 0.04 ms a real frame (0.15 with the search for
    DLSS FG), the mask's pass under 0.01 ms;
  - the capture card with a console on its home screen, HDR: both generators
    ran with the mask on and nothing failed; the mask held nothing, as hardly
    anything moves there.
- **HDR is arithmetic, not measurement.** The thresholds are steps of 8-bit
  SDR luma; on a PQ picture a step counts half as much of the code range
  (around a HUD's brightness PQ spends about half the range on the same
  contrast, and the PS5's noise in 10 bits is about half as large). The
  nearest thing to a test: the same clips re-encoded as PQ with white at 203
  nits (`fg_eval --hud ... --as-pq 203`) - the mask covers as much of the HUD
  as in SDR and is about as large; HUD error 5.54 as is, 1.39 with the mask
  (DLSS FG: 7.04, 1.20). Steps of 0.35, 0.7 and 1.0 gave 1.34, 1.39 and 1.52.
  No HDR game capture was scored.
- **Copies of the HUD beside the HUD** (the own generator; DLSS FG's frames
  do not show them). The mask puts the HUD's own pixels right, but the
  generator still carries HUD pixels along with the scenery: a faint second
  icon half a step from the icon.
  - *Kept, behind the switch `HudClean` (on):* the generator draws from the
    two real frames without the HUD the mask holds (`fg::HudMask::
    clean_frames`, CleanPS in `hud.hlsl`, between the motion search and the
    drawing: `fg::Interpolator::after_motion`). Under each HUD pixel of one
    frame goes what the scenery shows there in the other, a step along the
    motion; where that is HUD too, or no motion is known, a's scenery from
    the nearest place the HUD ends. The mask's pass then puts the HUD back.
    The warp shader is not touched, and with the mask off nothing differs
    (the seven short clips: 33.02 dB, every saved frame the same file).
  - *On the seven clips, mask everywhere, before and after:*

| | HUD's own pixels | | rest of the boxes | | whole frame, dB | |
|---|---|---|---|---|---|---|
| | mask | + `HudClean` | mask | + `HudClean` | mask | + `HudClean` |
| bf6_a | 2.33 | 2.36 | 16.85 | 16.74 | 16.47 | 16.47 |
| bf6_b | 0.77 | 0.80 | 18.64 | 18.64 | 16.10 | 16.10 |
| bf6_c | 0.90 | 0.94 | 12.11 | 12.14 | 20.52 | 20.52 |
| ds2_a | 2.11 | 2.11 | 4.38 | 4.41 | 36.11 | 36.10 |
| ds2_b | 0.51 | 0.51 | 1.08 | 1.08 | 36.19 | 36.19 |
| ds2_c | 0.51 | 0.54 | 1.47 | 1.47 | 34.61 | 34.60 |
| wardogs | 0.97 | 0.99 | 5.86 | 5.76 | 24.55 | 24.56 |
| mean | 1.16 | 1.18 | 8.63 | 8.61 | 26.36 | 26.36 |

    About 0.2 ms more per frame in between at 5120x1440 and 3840x2160 (the
    evaluator's 16-bit frames: two copies of a frame and two sparse passes);
    0.04 ms in the add-on's 1280x720 window.
  - *What it does:* in the frames looked at (bf6_a and wardogs, one pair
    each) the copies of what the mask holds are gone - the second weapon icon,
    the numbered markers, a status icon's green dot. The numbers hardly move:
    the copies are a small part of what is wrong with the scenery in the boxes
    (a hard box 11.15, DLSS FG with the mask 7.68 - DLSS FG's scenery there is
    7.59 as it is, the own generator's 8.74). The HUD's own pixels lose 0.02:
    a HUD pixel the mask does not hold, beside one it does, is no longer "the
    same all around in both frames" to the generator.
  - *What stays:* copies of HUD parts the mask does not hold. In bf6_a that
    is text over a cloud it does not stand out from: no evidence, no mask,
    and its copy shows where it lands on dark sky. Over the clip's sixteen
    pairs the mask never gets there; in play it has longer.
  - *Tried and dropped,* each scored on all seven clips (HUD's own pixels,
    rest of the boxes, whole frame; the mask alone is 1.16, 8.63, 26.36):
    - in the mask's pass, a pixel whose place in a and in b along the
      scenery's motion is HUD both times gets the real frame: 1.16, 8.64,
      26.36 - nothing;
    - there too, where one of the two places is HUD, the scenery from the
      other frame (the real frame if the pixel itself stands still): 1.19,
      8.77, 26.32. The scenery's motion per cell is not what the generator
      chose per pixel, and a guess laid over its frame costs more than the
      copies do;
    - the same with every pixel that gave evidence in this pair counting as
      HUD, held by the mask or not: 1.20, 8.90, 26.24;
    - the mask on from one pair of evidence instead of two (so it holds more
      of the HUD): 1.08, 8.84, and 26.14 for the whole frame - 130 000 to
      420 000 pixels outside the boxes are taken for HUD, on every clip. The
      second pair is what keeps scenery out;
    - a pixel that is on let go by its own change, not its 3x3's (a stroke of
      text over changing scenery; `hold_own`): 1.15, 8.63, 26.36, the mask 1 %
      larger. Off: too little either way in clips this short.
  (`fg_eval --hud ... --hud-set clean 0|1`, `carry 0..3`, `on_from 17`,
  `hold_own 1`.)
- **With time** (four of the clips cut again from the same places, 181
  frames, 90 pairs: 3 s of play, 6 s for bf6_a, whose recording is 30 fps;
  `fg_eval --hud ... --hud-late 12` scores pairs 5 to 16 and the last twelve
  apart). The mask in the clips above is always young; in play it has
  seconds.
  - *It gets there.* Share of the HUD's own pixels the mask holds, at pairs
    2, 10, 20, 30 ... 90 (the HUD's own pixels are now those unchanged over
    181 frames, a smaller set):

| | 2 | 10 | 20 | 30 | 40 | 50 | 60 | 70 | 80 | 90 |
|---|---|---|---|---|---|---|---|---|---|---|
| bf6_a | 79 | 89 | 100 | 100 | 100 | 100 | 100 | 88 | 100 | 100 |
| bf6_c | 96 | 100 | 100 | 100 | 100 | 98 | 100 | 100 | 100 | 100 |
| ds2_b (a menu) | 3 | 68 | 82 | 85 | 86 | 89 | 89 | 90 | 96 | 96 |
| wardogs | 74 | 94 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |

  - *Means of the four clips,* early and late:

| | as is | hard box | mask | mask + `HudClean` | DLSS FG | DLSS FG + mask |
|---|---|---|---|---|---|---|
| HUD's own pixels, pairs 5-16 | 2.29 | 0.70 | 1.09 | 1.10 | 1.01 | 0.79 |
| ... the last 12 | 1.48 | 0.53 | 0.54 | 0.56 | 0.70 | 0.52 |
| rest of the boxes, pairs 5-16 | 9.01 | 11.11 | 8.84 | 8.80 | 8.01 | 8.10 |
| ... the last 12 | 4.57 | 6.60 | 4.39 | 4.35 | 4.54 | 4.61 |
| whole frame dB, pairs 5-16 | 24.42 | 24.27 | 24.43 | 24.43 | 24.84 | 24.86 |
| ... the last 12 | 29.73 | 29.23 | 29.67 | 29.67 | 29.03 | 28.93 |

    Late, the mask is as good on the HUD as the hard box (0.54 against 0.53;
    bf6_a 0.74 against 0.71, 2.87 against 1.25 while young) and keeps the
    scenery in the boxes better than the generator alone.
  - *The mask grows on:* bf6_c from 132 000 pixels to 243 000, 44 % of them
    outside the boxes (10 000 pixels outside while young); ds2_b from 451 000
    to 764 000. With the mask everywhere the whole frame is then 0.1 dB under
    the generator's own on bf6_c and 0.2 on ds2_b (inside its box, a menu: on
    compressed video the later real frame is further from the true one than
    a mix of two is, 0.46 against 0.32 on what does not change). The mask
    inside the boxes costs nothing but that (29.71 dB).
  - *`HudClean` late:* the scenery in the boxes a little better (4.35 against
    4.39; bf6_a 8.62 against 8.77), the HUD's own pixels a little worse (bf6_a
    0.83 against 0.74), the whole frame the same. In bf6_a's late pair that
    was looked at the weapon numbers are doubled without it and single with
    it; the rest is alike. It stays on.
  - *What stays, late:* the copies of HUD parts that change - a counter's
    digits, the compass - which the mask lets go by design; and see-through
    panels. bf6_a's hatched icons are a half-transparent panel: over even
    sky the mask holds all of it (60 % of its box at pair 55), over cloud and
    ground its pixels change with what is behind and are let go (11 %, 11 %,
    1 % at pairs 30, 70, 85). The jagged patch seen there in a young mask was
    a mask caught half way; it is not holes to be closed, and in the four
    late pairs looked at it does not show.
- **Not known, and leftovers:**
  - the mask holds the HUD's own pixels and no more: a copy of a HUD part it
    does not hold is still drawn beside it (above; a hard box removes those);
  - a HUD part that itself moves or changes - a compass tape, a counter, a
    fading notice - is let go while it does, and drawn by the generator;
  - scenery that stands still on screen against moving scenery for two
    pairs with detail (a cockpit frame, a weapon held still) is taken for a
    HUD: it is then shown at the game's rate, which is what it does anyway;
  - other games' HUDs; a 16:9 picture with bars (the mask follows
    `PictureOnly`'s picture; not scored); GPUs of other makers (plain
    Direct3D 11 compute, not run on one); a back buffer in scRGB (not looked
    at there: no levels are known for linear values).
- **Also learned:** an F9 capture at x3 or x4 saved the frames of the pairs
  after its last real frame under that pair's names, over them. Fixed with
  this (the capture takes only the last pair's own).

## 15. The menu on a slow picture (11-18)

The in-game menu is drawn when a frame is presented. With dedup, a picture
of 5 to 20 new frames a second (a console's home screen, a game's menu) is
presented that often, a few frames in between added: short clicks went
unseen and dragging an area came in jerks. Repeats were asked of the app
only after a quarter of a second without a new frame.

- **The add-on cannot present by itself.** It runs inside mpv's presents, on
  mpv's thread; without a present it has no turn. More presents have to come
  from mpv, which presents what it is given: so the app is asked for the
  repeats (the link's `send_repeats`, as while frame generation is off)
  whenever the menu is open and fewer than about 60 frames a second are
  shown (on under 55, off from 58; a picture at 30 x2 or more gets none).
- **Frame generation goes on underneath.** A present that carries a repeat
  is left out of everything: no frame in between is made from it, and it is
  no real, generated or late frame in the log or the menu. Which present
  carries a new frame is told from the link's arrival times against the
  time of mpv's present: a new frame is presented a few milliseconds after
  it arrives, the repeat before it a capture interval earlier. The link's
  count alone would not do: a repeat still on its way through mpv (and DLSS
  5) when the new frame arrives would be taken for it.
- **Measured** (the HUD clip as a raw source in a 60 fps stream, x2, a
  1280x720 window on a 120 Hz screen, Tab posted to mpv's window):

| new frames a second | menu | real + generated | repeats | presents | "queue full" in 5 s |
|---|---|---|---|---|---|
| 20 | closed | 20.0 + 20.0 | - | 40 | 0 |
| 20 | open | 20.0 + 19.6 to 19.8 | 40.0 | 80 | 1 to 2 |
| 10 | closed | 10.0 + 4.2 to 5.6 | - | 15 | 0 |
| 10 | open | 10.0 + 3.8 to 4.8 | 50.0 | 64 | 0 |

  The menu was drawn on 714 and 717 of mpv's presents in 12 s (60 a second),
  besides the real frames the add-on presents. The hold's timing is the
  same open and closed (25.01 ms at 20 fps, no late frames). One or two
  frames in between in five seconds are lost to "queue full" with the
  repeats in the queue.
- **Seen on the way:** at 10 new frames a second only half the pairs get a
  frame in between, menu or not: a gap of 100 ms is `ResetMs`, and every
  other frame comes a hair later than that and starts the timing over. A
  source under about 11 fps wants a longer `ResetMs`; not changed here.
- **Not known:** DLSS 5 on (each repeat is a pass of its own, as while frame
  generation is off); the capture card (the runs were a raw source); mpv held
  up for longer than a capture interval with a repeat in hand, where one new
  frame would go unused for the frames in between (it is still shown).
