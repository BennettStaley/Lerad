# Lerad frame generation

<!-- SPDX-License-Identifier: MPL-2.0 -->

Frame generation inside mpv, with no second app running: the ReShade add-on
`zz-framegen.addon64` (this folder, Mozilla Public License 2.0) and
Lerad's dedup feeding it. This is the design document carried over from the
prototype (MPV-SW-Capture's FrameGen work, 2026-10-24 to 10-31): the
measurements below were taken there, with the same code. What was tried
on the way, the research rounds, the depth experiment and the dead ends are
in [docs/FRAMEGEN-LEARNINGS.md](../../docs/FRAMEGEN-LEARNINGS.md).

## What it does

1. **Dedup in `Lerad.exe`** (`[frames] dedup = 1`), which feeds mpv from the
   card: 10-bit P010 for HDR, 8-bit NV12 (or YUY2/UYVY) for SDR.
   A 30 fps game captured at 60 arrives
   as pairs of identical frames (A A B B). With `--dedup` only new frames are
   written to mpv, so mpv, the DLSS 5 pass and the frame generator all see the
   game's real 30 fps. Repeats need not be bit-exact: the PS5 adds faint
   noise to every refresh (a quarter of the pixels move by 1-3 8-bit steps),
   so frames are compared by 16x16 block averages, in which the noise
   cancels, plus single pixels that moved far (a HUD digit). Repeats are
   still sent every 200 ms so menus and the OSD
   keep drawing on a still picture. `--dedup` also opens a link to the add-on
   (`framegen_link.h`) with each new frame's arrival time and how many new
   frames mpv had to drop.
2. **`zz-framegen.addon64`**, a ReShade add-on that runs last, after the
   DLSS 5 pass. For each real frame B it shows a frame between the previous
   real frame A and B, then B half a frame later: 30 fps becomes 60, 60 becomes
   120. With `Multiplier=3` or `4` (an option, never the default) it shows two
   or three frames between A and B, a third or a quarter of a frame apart:
   30 fps becomes 90 or 120.

The frame in between is **motion-compensated** (`Method=motion`): the add-on
finds how every part of the picture moved from A to B and draws each pixel
from where it was in A and where it went in B. `Method=blend` gives the old
50/50 mix (a double image on motion) for comparison.

## Use

Frame generation is on by default (`lerad.ini`: `[framegen] enabled = 1`)
and needs dedup (`[frames] dedup = 1`) and ReShade in `engine\`. **F8**
toggles it while playing (mpv's OSD says ON/OFF); the next start follows
`lerad.ini` again. With dedup off, F8 only says it needs repeated frames
dropped, and where the window has that.

While frame generation is off, mpv gets every frame anyway, repeats too
(`[framegen] all_frames_when_off = 1`): the add-on asks Lerad for them
through the link, which keeps telling new frames from repeats, so F8 back on
returns to new frames only at once (`logs\lerad.log` notes each switch).
`all_frames_when_off = 0` keeps new frames only while frame generation is off
as well (DLSS 5 then runs once per game frame instead of on every captured
one). Measured at 30 fps with VRR, mpv's present to the screen: 6.77 ms with
new frames only and 5.72 ms with every frame at the driver's default power
setting (the GPU keeps its clocks up at 60 frames a second); with NVIDIA's
"Prefer maximum performance" for mpv both are 5.6-5.7 ms - but that setting
made every frame cost the GPU more at 60 fps (below), so the default is the
better choice.

The repeats are asked for in one more case: while the in-game menu (Tab) is
open - also while an area is drawn or dragged with the menu faded - and
fewer than about 60 frames a second are shown (a still picture, or 5 to 20
new frames a second: a console's home screen, a game's menu). The menu is
drawn when a frame is presented, and at that rate short clicks went unseen
and a drag came in jerks. mpv then presents every captured frame and the menu
is drawn on each. Those presents are not frames of the game: the add-on
leaves them out of everything (`menu_repeat` in `framegen_addon.cpp`) - no
frame in between is made from one, and none counts as a real, generated or
late frame; `ReShade.log` has a line of its own for them ("the menu was open
on a slow picture: 40.0 repeats of it a second shown as well ..."). Which
present carries a new frame is told from the link's arrival times and the
time of mpv's present. Measured with a 20 fps picture and x2 (a 1280x720
window, 120 Hz): 40 presents a second with the menu closed, 80 with it open
(the menu drawn 60 times a second on mpv's presents alone); real frames 20.0
either way, frames in between 20.0 closed and 19.6-19.8 open (one or two in
five seconds lost to "queue full"), the hold's timing the same (25.01 ms).
A picture shown at 55 a second or more gets no repeats.

Settings are in `ReShade.ini`, section `[LERAD_FRAMEGEN]`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Frame generation on at start (Lerad writes it from lerad.ini). |
| `ToggleKey` | 119 | Virtual key code of the toggle (119 or 0x77 = F8). |
| `CaptureKey` | 120 | Saves the frames around this moment (120 = F9, see Captures). |
| `DedupWhenOff` | 0 | 1 = new frames only while frame generation is off too (lerad.ini `all_frames_when_off = 0`). |
| `MaxOutputHz` | 0 | Frames are only added while the result stays at or under this; 0 = the display's refresh rate (lerad.ini `max_output_hz`). A limit below the screen's rate is exact; the screen's own rate has 5 % of tolerance. |
| `Multiplier` | 2 | Frames shown per source frame, at most: 2, 3 or 4 (lerad.ini `multiplier`). The most that fits is used. |
| `ResetMs` | 100 | A longer gap between frames starts the timing over. |
| `RequireDedupLauncher` | 1 | Only run with the link from Lerad's dedup. |
| `Method` | motion | `motion` = motion-compensated, `blend` = 50/50 mix (lerad.ini `method`; the menu switches it). |
| `Quality` | high | `high`, or `fast`: fewer candidates per pixel, about half the drawing cost (lerad.ini). |
| `Backend` | own | What makes the frames in between: `own`, or `dlss` (NVIDIA DLSS Frame Generation, where the GPU has it and its model is in the engine folder; lerad.ini `backend`). The app's name, which this was called before, is read as `own`. |
| `Keep` | (empty) | Areas the frames in between leave alone (lerad.ini `keep`; here with `/` and `\|` for its `,` and `;`): `r:2/80/20/15\|e:85/5/12/20`. With `/hud` after its numbers an area only says where to look for a HUD (below). |
| `Hud` | 0 | 1 = find the HUD everywhere: the frames in between show the nearer real frame wherever a HUD is found, with no area drawn (lerad.ini `hud`; below). |
| `PictureOnly` | 1 | A switch (below). With black bars around the picture (a 16:9 game on a 21:9 screen), frames are generated from the picture alone; mpv says where it is. Better scenery at the picture's edges and less GPU time, but a HUD near those edges loses the bars' protection (docs/FRAMEGEN-LEARNINGS.md, 4.6): `StillHud` is for that, keep the two together. Does nothing without bars. |
| `EdgeRules` | 1 | A switch. The strip along the frame's edges that only one frame shows in a camera turn is drawn along the turn's motion (same section). |
| `StillHud` | 1 | A switch. Icons and digits drawn over fast-moving scenery are held where they are: the cells that hold detail which stands still while they move get no motion (docs/FRAMEGEN-LEARNINGS.md, 4.7). Gives a HUD near the picture's edges back what `PictureOnly` takes from it, and acts on a frame without bars too. The scenery right beside such a HUD (a cell, 16 px) stands still with it. |
| `HistorySearch` | 0 | A switch, and a test: 1 = the motion search also starts from what each block did over the pair before (docs/FRAMEGEN-LEARNINGS.md, 4.5). Better offline on some captures, little either way on others. |
| `HudClean` | 1 | A switch. With the HUD mask on (`Hud=1`, or an area with `/hud`) and the app's own generator: the frame in between is drawn from the two real frames without the HUD the mask holds, so no faint copy of an icon is carried along with the scenery (docs/FRAMEGEN-LEARNINGS.md, 14). About 0.2 ms more per frame in between at 4K. Does nothing while the HUD is not looked for. |
| `StepDown` | 1 | A switch. Where x4 or x3 fits the screen but cannot be held there, one fewer is shown for every game frame for a while, so that each frame stays the same time ("A multiplier that cannot be held", below). 0 = always the most that fits the screen, as before 2026-11-18. |
| `DebugMarker` | 0 | Magenta square in the top-left corner of generated frames (the picture's corner with `PictureOnly`). |
| `LogStats` | 1 | A line in `ReShade.log` every 5 s (see below). |
| `MpvPipe` | lerad | mpv IPC pipe for the OSD message. |

The six switches are not in `ReShade.ini` unless one differs from its
default: the add-on writes the other keys there at its first start, but a
switch only when the panel (below) is used to choose against the default,
and it takes out one that says what the default says. So a later version
can change a default for everyone who did not choose otherwise (the first
three were tests, off, until 11-16: docs/FRAMEGEN-LEARNINGS.md, 4.8).
To set one by hand, add the line (`StillHud=0`). `ReShade.log` has what is
in use at start: `switches: PictureOnly=1 EdgeRules=1 StillHud=1
HistorySearch=0 HudClean=1 StepDown=1`.

The six switches, `DebugMarker` and `LogStats` can also be changed
while playing: ReShade's own overlay (Home), page Add-ons, under "Lerad
Frame Generation". The panel says what the generator is working on (the
whole frame, or the picture and where it is), each checkbox works at once
and is remembered in `ReShade.ini`. `PictureOnly` makes the generator anew a
quarter of a second later. It is a page for trying things on the scene in
front of you; `Enabled` and `Quality` are not on it, as Lerad writes those
from lerad.ini at every start (its own menu, Tab, has them).

The panel is drawn with ReShade's Dear ImGui, the menu with the add-on's own
(src/overlay). `reshade_ui.cpp` keeps them apart: it calls ReShade's
functions through the table ReShade hands out and is the only file that
includes `reshade_overlay.hpp`. That header's inline functions carry Dear
ImGui's own names (in one DLL with the real ones the linker would keep one
of each for both) and it is written for the docking branch, which
third_party/imgui is not; so they are left out and two type names are
declared by hand. A ReShade without the overlay, or with another Dear ImGui
version, gets no panel and the switches stay in the file.

## When it generates

A frame in between is made only when

- twice the source rate fits the display (`MaxOutputHz`, or its refresh
  rate). With `Multiplier` 3 or 4 the most that fits is used (30 fps x4 and
  60 fps x2 on a 120 Hz screen). A rate at the edge does not switch back and
  forth: what is running gets 3 % more room, and going up again waits two
  seconds (`MultiplierGate` in `fg_pacing.h`),
- fewer than two presents are still queued for the screen ("queue full":
  the game frame is then shown alone). When that keeps happening at x3 or
  x4, one fewer is shown for every game frame ("A multiplier that cannot be
  held", below),
- the source is not paused (a still picture sends only keepalive repeats),
- mpv keeps up. New frames dropped upstream right after a generated frame
  (with mpv at its usual pace) pause generation for 5 s, then 10, 20 ... up to
  60 s each time it happens again; 30 s after the last pause and the last such
  drop reset that. Drops of mpv's own - across a stall far longer than the
  add-on's wait (DLSS 5 starting, shaders recompiling), or while not
  generating - only make it wait until mpv has gone a second without dropping.
  It also waits 2 s after mpv starts. At x3 and x4 such drops first take one
  frame off every game frame (below); the pause is what is left at x2.

The source rate comes from the link (the card's arrival times), not from
mpv's frames: while generating, mpv runs at the add-on's pace, so only the
card's clock can tell that the display is too slow for twice the source rate.

## What the screen does to the pacing

The frames are spaced by time (the hold, below): each is let go one step
(the source interval divided by the multiplier) after the one before. What
that looks like depends on the screen - the one mpv's window is on, which
the add-on looks up every second (`fg_vblank.h`): the window can be moved,
and screens are plugged in or switched off while it runs. Its refresh rate
is read from Windows then. Its VRR state is what Lerad.exe found for every
screen at start (`LERAD_SCREENS`, `fg_screen.h`: on, off or unknown per
screen); a screen that was not there then, or another monitor under the
same output name, is "unknown". The add-on judges the result
(`judge_cadence` in `fg_pacing.h`) and the menu says so. The log says where
the window is, and each change ("the window is now on screen 2 (...)").

| Screen | Example | What happens | The menu |
| --- | --- | --- | --- |
| VRR, output inside the range | 30 -> 60 or 90 on 120 Hz | Each frame is shown when it is let go: even (measured with x2, see Measured) | nothing |
| VRR, output at the refresh rate | 60 -> 120, 30 -> 120 (x4) on 120 Hz | The step equals the refresh time: a frame let go a step after the one before is shown at once or a fraction of a millisecond later. A source a little faster than half the refresh rate fills the queue, and one frame in between is left out now and then ("queue full") | "120 fps fills this 120 Hz screen: VRR has no room left" |
| Fixed refresh, a multiple of the output | 30 -> 60 on 120 or 240 Hz | Frames land on whole refreshes. A frame let go a whole number of refreshes after the one before was done arrives just after its refresh and waits for the next: measured 5 and 3 refreshes instead of 4 and 4 (below) | "VRR is not engaging" if VRR was expected |
| Fixed refresh, not a multiple | 60 -> 120 on 144 or 165 Hz | Uneven whatever is done (1, 1, 1, 1, 2 refreshes) | "Uneven on this screen ... Set the screen to 120 Hz, or turn VRR on" |

Whether VRR really engages cannot be asked of Windows. The add-on counts
the refreshes of the screen the window is on - a thread waits for each
vertical blank of that screen's own video source
(`D3DKMTWaitForVerticalBlankEvent`, the call under DXGI's `WaitForVBlank`;
`fg_vblank.h`) - and reads the count once a second (`RefreshWatch` in
`fg_pacing.h`). The count and what was made of it start over when the
window is on another screen or the screen's mode changes. (Until 2026-11-18
it was the swap chain's `SyncRefreshCount`, which follows Windows'
compositor for a composed window: the main screen's 240 on the 120 Hz
screen beside it.) When
the screen is seen refreshing at its full rate for three seconds while
clearly fewer frames are shown (45 or more a second, under 90 % of the
rate), it is refreshing at a fixed rate - VRR is off for windows in the
driver, the window is not focused, or Windows composes it. The log's 5 s
line carries the reading ("seen refreshing at 240.0"), the log warns once
when VRR was said to be on, and the menu tells the user. That a screen with
VRR engaged reads lower (the refreshes following the frames) has one
measurement behind it (2026-11-18, the count taken from outside mpv with
a small probe kept outside the repo): a 120.000 Hz screen read 119.86-119.90 while
Lerad showed 59.94 x2 on it with VRR on, and 120.00 once mpv was closed.
That is at the top of the range; well below it (30 -> 60 on 120 Hz, where
the warning matters) it is still to be seen, as are two screens at once and
a window moved between them.

Measured 2026-11-12 on an RTX 5090, a 240 Hz screen, a 1280x720 window that
did not have the focus ("Composed: Flip" in PresentMon, so a fixed 240 Hz
whatever the screen can do), a synthetic 30 fps source, DLSS 5 off:

| Setting | Let go (CPU, from the trace) | On screen (PresentMon) |
| --- | --- | --- |
| 30 fps x2 | real frame 16.68 ms after the frame in between was done, sd 0.00 | 60.0 fps; 5 and 3 refreshes alternating (20.8 / 12.5 ms), sd 4.4 ms |
| 30 fps x2, `LERAD_FRAMEGEN_LEAD_MS=5` | 11.68 ms after | 60.0 fps; 78 % of frames 4 refreshes (16.7 ms), sd 2.7 ms |
| 30 fps x3 | steps 11.12 and 11.11 ms, sd 0.01-0.05 | 90 fps; 3, 2, 3 refreshes (12.5 / 8.3 / 12.5 ms): 90 does not divide 240 |
| 30 fps x4 | steps 8.34, 8.32, 8.32 ms, sd 0.13 | 119.3 fps; 71 % of frames 2 refreshes (8.3 ms), sd 2.7 ms; other runs 107-109 fps with 19-24 "queue full" in 5 s |
| 60 fps x2 | 8.35 ms after, sd 0.01 | 98-101 fps, 94-107 "queue full" in 5 s (the composed window, as in FRAMEGEN-LEARNINGS.md) |

`LERAD_FRAMEGEN_LEAD_MS=<ms>` lets every held frame go that much earlier.
It is a test switch for fixed-refresh screens, where it puts frames back on
their refresh (the second row); with VRR it would make them uneven.

## A multiplier that cannot be held

What fits the screen by its numbers is not always what the screen takes.
With x3 and x4 all of a game frame's frames must be out before the next one
comes. A window Windows composes shows one frame a refresh, so 30 fps x4 on
120 Hz leaves it no spare one: presents are still queued when the next game
frame comes, and that one is shown alone ("queue full"). Measured
(2026-11-18, a 1280x720 window without the focus on a 120 Hz screen, a 30
fps source): every other game frame, so four frames 8 ms apart and then one
that stays 33 ms, 75 frames a second instead of 120 - which looks worse than
90 even ones. A GPU that is late with the first added frame (DLSS 5 taking
most of the interval) ends the same way.

`StepDown=1` (the default; `StepDown` in `fg_pacing.h` is the rule, with its
numbers and tests) takes one frame off every game frame instead:

- **Down.** Of the game frames that would get their frames at x3 or x4, 4
  shown alone within 2 s, the first and the last of them at least half a
  second apart: one fewer from the next game frame on (x4 to x3, x3 to x2).
  A hiccup (a few in a row, then none) and a frame left out now and then (a
  screen at the top of its VRR range does that) are below it. mpv dropping
  the game's frames while generating at x3 or x4 does the same at once,
  instead of the pause for all frame generation.
- **Again.** The one above is tried after 20 s. For 3 s after a try, 2 game
  frames shown alone are enough to go back down (a tenth of a second of
  uneven frames, measured), and the next try waits twice as long: 40, 80,
  160, then 300 s each time. A try that held a minute was not a failed one;
  the waits start over.
- **At once.** It is tried as soon as something changes that may have been
  the cause, but not before the lower one has been in use 5 s: the window
  (its place, size or style: the window mode), the screen or its mode, the
  picture's size or place in the back buffer, the source's rate (a steady
  one a tenth off; not a rate that moves by itself), ReShade's effects
  starting or stopping (DLSS 5 switched). A setting of frame generation
  changed in the menu or with F8 (on or off, "Up to", "Frames by", quality,
  method, "find the HUD everywhere") forgets everything: the multiplier is
  chosen as at start, and the waits start at 20 s.
- **x2** is never stepped down from. Frames left out there are said in the
  menu as before ("104 of 120 fps"), and mpv not keeping up is what the
  pause is for.
- **Why**, for the menu and the log: the GPU when mpv dropped frames, or when
  most of the game frames that did get their frames had them let go early
  for the next one (the hold's "let go early" and "already due when done":
  the first added frame was done too late for the others to keep their
  step); else the screen.

The menu says it as a warning: "x3, not x4 / The screen takes frames late at
x4, so every game frame gets two added frames instead of three. Windows may
be composing the window." with "Switch to full screen", or "The GPU cannot
keep up at x4 ..." with "Use Fast quality". `FramegenStatus` carries it
(`Reason::SteppedDown`, `step_from`, `step_cause`, `seconds_left`), and the
panel on ReShade's Add-ons page has the switch and a line with the seconds
until the next try. The multiplier still goes through `MultiplierGate`: on
a screen known to refresh at a fixed rate, x4 that cannot be held becomes
x2 where x3 would not fall on whole refreshes.

It works by counting game frames and what they got, so it is the same for
either generator (seen with both). Whether a window holds 30 fps x4 on 120
Hz differed from run to run on the PC it was measured on (it started out
losing every other game frame in 7 runs of 12; in the others it lost one
game frame's frames every 3 s, which is under the rule's limit, and one of
those went over after 27 s): the source's 30 and the screen's 120 are not
the same clock, and where the presents fall between two refreshes moves.
Not seen: a window flipped straight to the screen, VRR engaged, a console
in motion on the card, and the GPU as the cause (the rule's tests and the
menu's demo state only).

## Logs

`ReShade.log`, every 5 s:

```
30.0 real + 90.0 generated fps (x4 of up to x4) | interval 33.33 ms (source) | display
120.00 Hz (seen refreshing at 120.0), VRR on, cap 120 Hz
| 0 late frames, 0 dropped upstream after a generated frame | not generated: 0 too fast,
0 queue full, 0 source paused, 0 backing off, 0 mpv settling, 0 effects mix
| mpv present 0.04 ms avg, 0.11 max | real frame held on the GPU 16.68 ms avg after the
frame in between was done (done 9.41 ms avg after the add-on drew it), let go 0.017 ms late
avg, 0.029 max; of 151: 151 timed (0 of them let go early for the next frame, 0 already due
when done), 0 without the done signal, 0 with a later frame (queue full, resize); queued in
10.00 ms max
```

While a multiplier is stepped down (above), the line begins `30.0 real +
60.0 generated fps (x3 of up to x4, stepped down from x4 for the screen, x4
tried again in 12 s)`, and each change has a line of its own:

```
x4 cannot be held (the screen takes frames late: 9 game frames in the last 2 s went
without their added frames, presents were still queued); x3 for the next 20 s
x4 is tried again (its time came)
x4 cannot be held (the screen takes frames late: 2 game frames in the last 2 s ...); x3
for the next 40 s
x4 is tried again (the window changed)
frame generation switched: no longer stepped down to x3, the multiplier is chosen anew
```

After "interval", a source whose rate moves reads `(source, 40-57 fps,
variable)`, and one with a steady rate in uneven steps (45 fps inside a 60 Hz
signal) `(source, uneven steps)`. With x3 and x4 the "held" figures count
every held frame (the later frames in between and the real one), each about
one step.

"mpv present" is how long mpv's present of the generated frame blocked. The
real frame is presented right after it but held on the GPU, and a thread of
the add-on lets it go half a source interval after the frame in between was
done (that is when VRR shows it): "held ... after the frame in between was
done" is how long it actually waited, "let go late" how far after its time
the thread did it. Of the held frames, "timed" ones were let go by that
thread; "let go early for the next frame" were let go before their half so as
not to hold up the next frame's DLSS 5 pass (it queues behind the hold),
"already due when done" had their deadline pass before the frame in between
was even done (the GPU is at its limit); "without the done signal" were let
go after 200 ms without one (a GPU reset); "with a later frame" were let go
together with a newer one (the hold's queue was full, or the window
resized). "Queued in" is how long the copy and present of the real frame
took on mpv's thread (Windows makes the present wait until the frame in
between is drawn). "None in this window" means nothing was generated. Without
the hold - no Direct3D 12, which the log says at the first generated frame,
or `LERAD_FRAMEGEN_PACING=present` for comparisons - the add-on sleeps on mpv's
thread until half an interval after mpv's present and the line ends
`real frame out late by ...` instead.

In the trace (`LERAD_FRAMEGEN_TRACE`), `real_ms` is when the frame was let
go on the CPU: the release of the hold, or (without it) the end of the sleep
before its present; -1 when it went out with a later frame. `frame` and `of`
say which of the source frame's frames the line is (2 of 2 is the real frame
of x2; with x4, lines "between" 2 and 3 of 4 come before "generated" 4 of
4). Neither is when it reached the screen. For that the trace also has
`#screen` lines, written by a thread that reads the swap chain's frame
statistics every half millisecond: time, presents shown so far, the refresh
the latest was shown on, refreshes so far, and when the latest refresh was.
On a fixed-refresh screen the difference between two lines' refresh numbers
is how many refreshes a frame stayed up; it matched PresentMon's
`msBetweenDisplayChange` in the runs above. PresentMon itself runs without
administrator rights for members of the "Performance Log Users" group.

`logs\lerad.log`:

- `dedup: 28.8 new frames/s of 57.5 captured (changes: repeats up to 0, new
  frames from 2556, new from 4; repeats' noise up to 5 a sample, 1.0 a block;
  threshold 3)` (a PS5 game at 30 fps): a change is a 16x16 pixel block whose
  average moved by more than the threshold, or one sampled pixel that moved by
  more than 8 times it, from the last frame sent; a frame with 4 changes is
  new. The threshold (`--dedup-threshold`, default 3) is in 10-bit steps for
  NV12 too: one 8-bit step is 4. The noise is how far the repeats' samples and
  block averages moved: 0 for a bit-exact source like the Switch 2, about 8
  and 1.5-1.8 for the PS5 in NV12 (5 and 1.0 in P010). If repeats come close
  to 4 changes, or the block noise close to the threshold, raise it. Near 60
  new frames/s from a game meant to run at 30, with new frames from thousands
  of changes, means the picture really changes on every refresh (a 40 fps or
  VRR mode, say), not that repeats slip through.
- `mpv fell behind: 1 of 150 new frames dropped in the last 5.0 s`: mpv did
  not keep up, with or without frame generation. The first seconds always
  show some (`... dropped before mpv started reading`, then DLSS 5 starting).

## How it works

- `reshade_present` (the back buffer is final: the DLSS 5 pass ran during
  ReShade's effects): copy B out, draw the frame between A and B into the back
  buffer, return. mpv's own present shows it.
- `finish_present` (ReShade's locks are released): copy B back into the back
  buffer and present it through the original swap chain, held on the GPU: the
  queue gets "the frame in between is done" (a D3D11 fence), the copy, a wait
  for a second fence and the present, and mpv's thread goes on. A thread of
  the add-on sees the first fence reach the frame, sleeps until half the
  source interval later - at most until just before the next frame's effects
  are due - and moves the second fence on from the CPU (a Direct3D 12 fence on
  the same GPU, shared with D3D11: D3D11 cannot signal one from the CPU). With
  VRR that puts B on screen half an interval after the frame in between.
  Without the hold (no Direct3D 12, or `LERAD_FRAMEGEN_PACING=present`) it
  sleeps until half the source interval after B arrived and presents B then.
  That present bypasses ReShade, so the DLSS 5 add-ons still see exactly one
  frame per real frame.
- Two frames are only mixed when ReShade's effects ran for both or for neither.
- More than one frame in between (`Multiplier` 3, 4): `reshade_present` draws
  the first at 1/3 or 1/4 of the way from A to B (`fg::DrawOptions::t`; the
  shaders take any t). `finish_present` then, for each later one and last for
  B: signals that the frame before is done, draws it (or copies B) into the
  back buffer, queues a wait and presents. The GPU draws each while the one
  before is on screen. The hold's thread times all of them from when the
  first frame in between was done, a step apart; if the next source frame's
  effects are due before the last would go, the steps shrink evenly
  (`squeezed_step`). Each present of mpv's thread waits until the frame
  before went out, so that thread is busy until the second to last frame is
  let go ("queued in": 9 ms at 30 fps x3, 13.5 ms at x4). The hold's thread
  must never call into the swap chain: with x4, reading its frame statistics
  there waited for a lock mpv's blocked present held, while that present
  waited for the thread's release - a one-second stall each time.
  Each frame in between runs the whole interpolation again (the search too,
  which does not depend on t: searching once and drawing per t would save
  about a fifth of each added frame, and needs `fg_interp.h` split in two).

The frame in between (`fg_interp.h`, shaders in `flow.hlsl`):

1. Luma pyramids of A and B, from half resolution down (up to 7 levels).
2. The motion both ways (A to B, B to A), coarsest level first: every 8x8
   block is searched for around twice the estimate from the level above, the
   best offset refined to a fraction of a pixel, then a 3x3 vector median. The
   search follows the design AMD documents for FidelityFX Optical Flow (MIT),
   written for shader model 5. Each block is also searched for around no
   motion, and what that finds is taken if it fits clearly better (at most
   half as badly) than what the search around the estimate found: a
   character the camera follows or turns around stays nearly still on screen
   while the scenery sweeps past, and a small part of it - the head - is lost
   to the scenery on the coarse levels; the search near the scenery's motion
   never finds its own. Without this, the head took the scenery's motion, and
   the frame in between showed faint copies of the hair and face beside it.
   Both are judged after their search: judged before it, the estimate (a few
   pixels off) lost to no motion on even stone, and a pillar the camera
   swept past got a hole. Trying the neighbouring blocks' estimates as well
   was worse: on stairs, a repeating pattern, a wrong one fits too and spread
   into blotches.
3. The motion at the frame in between, per 16x16 pixels: of the motions found
   nearby in both directions (and those found where each says the content came
   from and went to), the one under which A, shifted back half way, and B,
   shifted forward half way, agree best. (With `StillHud`: a cell that moves
   but holds edges which are the same in A and B where they are - not along
   the motion, and not the same again where the motion takes them - holds a
   HUD over the scenery. It and the cells right around it get no motion, here
   and as A's and B's blocks, so step 4 treats the HUD as it treats anything
   that stands still.)
4. Each pixel tries the motions of the 3x3 cells around it and none, judged
   on a small cross of five pixels by how well A and B agree there; a motion
   several cells share is sampled once and counts as many votes. Where only
   one frame shows the right thing (background a moving object uncovers, the
   edge of the screen in a pan) a motion may also be drawn from that frame
   alone - if that frame's own motion agrees, and not where that frame's pixel
   plainly stands still (a character the camera turns around). The options
   are mixed by how well they fit, as softly as the best one fits badly: where
   one fits well the result is that one (no faint copies of the others - the
   ghosting around characters), where nothing fits (UI fading or pulsing over
   moving scenery, glass, smoke) a soft blend instead of blocks. Where nothing
   fits and no one-frame option explains the pixel either (lightning, sparks,
   an attack faster than the search), the mix may go softer still: a smear
   like motion blur instead of misplaced pieces. Pixels that are the same all
   around in both frames (black bars, a still HUD) are drawn as they are,
   without a search. And what a pixel shows in both frames it shows in
   between: near something that stands still (the character the camera
   follows or turns around, a HUD), a moving option that would bring
   something else to a pixel where both frames show the same pays for it.
   Without that, the camera turning around a character showed the ground
   through the top of the head - the spiky hair turns a little between the
   frames, and the ground beside the head, carried along with the scenery,
   fitted better. Something moving further than its own width past a still
   thing (a fast spear tip) is drawn a little less in between instead.
   Beside a character the camera turns around fast, the strip only one frame
   shows (half the scenery's step wide, 65 px in a quick orbit) has to come
   from the right frame: the other one's sample lands on the character, on a
   spike of hair poking out of blocks that carry the scenery's motion, and
   costs the same - mixed in, it drew a faint copy of the hair tips beside
   the real ones (three tips where there are two) or a faint second outline
   of the head. So a one-frame option along the scenery's motion is ruled out
   where its sample lies on (or within up to 32 px of) a block of its frame
   moving under a quarter as fast, while the other frame's sample is clear
   of one. Only where most cells around move with the scenery, so a spear
   swung across still scenery or a character running past is not touched,
   and only one way round: where both samples or neither lie near something
   slow (smoke, an effect), nothing changes.
5. Nothing in step 4 may change at the 16-pixel cell edges: the set of cells a
   pixel tries moves on by a row or column there, so each cell's say fades out
   over its last half cell instead of stopping, and a frame's own motion is
   taken between its blocks, not per block. Otherwise wherever cells disagree
   (characters, effects) the frame in between shows the cell grid as blocks.
6. The character as a layer (`layer_enable`, 2026-10-29). Steps 1-5 judge
   each pixel by which motion makes A and B agree. Thin parts of the player
   character - the spikes of the hair - deform as it runs or turns, so no
   motion fits them well, while the smooth floor behind them fits the
   scenery's motion almost perfectly: the frame in between showed floor where
   the spikes were ("the head phasing out"). No per-pixel rule can fix that
   (four were tried: anything allowed to paste the hair also pastes repeating
   texture one step off). So the character is found as a whole and drawn on
   top:
   - The background motion is modelled locally (an affine fit per 128 px
     block around the most common motion of the cells - perspective makes a
     floor's motion vary across the frame). A cell is a candidate when its own
     motion differs from that by more than 6 px and fits clearly better
     (by 0.02 luma).
   - Candidates are linked into components; one compact component is kept
     (12-1500 cells, filled at least a quarter of its box, near the centre,
     not mostly black, at most 40 cells wide, moving at most 40 px a frame -
     wider or faster ones are a close-up body or an effect, and then there is
     no layer for that frame). Its motion is searched directly on its cells.
     A deletion vote (does the scenery slide under it?) drops it when the
     order looks wrong. In a pan there is one motion and nothing runs.
   - In each frame, a pixel mask grows from the component by colour - closer
     to the layer's local colour than to the background's, or carried onto
     the other frame by the layer's own motion - within 4 cells, and only
     where the background motion does not explain the pixel (checked within
     1.5 px). Enclosed holes are filled; nothing is dilated or closed (that
     would fill the gaps between letters).
   - The masks are moved to the frame in between with the layer's motion. The
     layer is drawn from frame A only, replacing what step 4 drew with the
     background's motion: the spike comes back solid, in A's shape (half a
     frame behind its true shape - the user's choice over a missing or
     doubled spike). B's copy of a deformed spike lands a few pixels off, so
     B alone is never drawn there.
   - What counts as "the background's motion" there: within 6 px of the
     local model, or within 30 % of how fast the background moves past the
     layer if that is more (`layer_bgnear`, 2026-10-29). In a fast camera turn
     around a running character (the floor moving ~115 px a frame) the floor
     beside the head moves 92-128 px across 300 px of perspective and the model
     is 10-15 px off; with 6 px alone the floor drawn over the top of the head
     was left in place (20261029-020140-625, first generated frame). Counting
     anything nearer the background than the layer took a swinging arm too
     and pasted flecks on it.
   - Fast turns while running (2026-10-29 evening, captures 20261029-1817*,
     floor ~90-140 px a frame):
     - A component too fast or too wide to be the character (a far
       pillar's parallax) no longer cancels the layer for the frame; it is
       left out of the choice and the next one (the head) can win
       (`layer_select_gates`). Components far off the middle (HUD, minimap)
       still cannot.
     - The background model also counts plain cells, at half weight
       (`layer_bg_flat`). A fast turn blurs the floor until hardly any
       cell has detail, and the detailed ones were the character's: the
       model said the background stood still beside the head, and half
       the hair was left out of the mask in a straight line.
     - The "another motion nearby explains this pixel" rule no longer asks
       the layer's own cells (`layer_nb_own`): a head turning on a running
       body cut 16 px squares out of its own mask.
     - Off the layer, an option that does not move with it but samples the
       layer in one frame pays 1 (`layer_ghost`): in a fast turn the
       scenery's motion picked up the head from one frame and drew it again
       half a scenery-step away (the afterimage beside the hair).
     On the old captures this keeps every score, halves the ghost count on
     the running set (338 to 184 px) and cuts it on the orbits (6349 to
     5534); the thin hair still survives in 15 of 15 frames. One pair it
     cannot fix: a dodge where the body moves ~60 px one way and the hair
     another (20261029-181808-872, last frame) - no single layer motion
     fits it.
   - Other characters near the player (2026-10-29 night, captures
     20261029-1853*..1855*, a fast turn in a busy town square): the choice
     often took an NPC walking beside him, and the player's hair got no layer
     (a white smear of the floor through it for one frame). Now:
     - The player's head often comes as small pieces (the head top, 7-18
       cells, beside the rest), each losing to the NPC on size. A component
       is scored and drawn together with the small, clear pieces (at most 24
       cells, margin at least 0.1) within 2 empty cells of it that move
       alike (`layer_merge`; linking all minority cells that far apart ran
       down the body into the floor and lost the needle).
     - Off the middle across, a component crossing the screen faster than
       10 px a frame scores less, (10 / speed)^2 (`layer_still`,
       `layer_still_pow`, `layer_still_dx`: from 100 px off the middle, full
       at 250) - the camera follows the player, NPCs beside him walk at 8-25
       px 260-450 px off it. In the middle the faster part is as often the
       player's own head running ahead of the camera.
     - Farther than 250 px off the middle across a component scores less
       (`layer_hcentre`); not more than that - the orbit captures have the
       player 370 px off it.
     - A layer with no inner cells searched every offset at cost 1e9 and
       took the first, 16 px off both ways; it keeps its cells' mean now
       (`layer_refine_edge`). (20261029-185417-238 pair 1's 16 px was a
       valley along a horizontal edge, not a minimum; rejecting edge optima
       too, `layer_refine_edge 2`, changed nothing measurable.)
     The hair hole score on the 185* captures falls from 2166 to 1794 px (the
     rest is 185517 pair 2-3, where the player was already chosen);
     nothing else changes. Keeping the last pair's layer as a prior was
     tried and dropped: when the last pair had chosen the NPC it kept it.
      These three only while the dominant motion is at least 6 px a frame
      (`layer_select_dom`: the camera follows the player; in fights with a
      still camera they picked effects and pasted blocks into a fire ring
      and foliage across lightning), and only a slow component near the
      middle across may take pieces (`layer_merge_core`: a fast part off
      the middle pasted a doorway beside a shoulder and a square at a
      crossguard). Known costs: an NPC beside the player now often has no
      layer, so her hair top can go soft (185414, 185417), and an NPC's
      shadow beside the head gets a hard pale edge in 185345 pair 3-4.
   On the four running captures the thin top of the hair now survives in all
   15 measurable generated frames (5 of 15 before; `eval\needle_score.py` on
   `fg_eval --adjacent` output, head regions in `eval\head_rois.json`), at about +0.07 ms in a pan and +0.15-0.3 ms with a
   character.

The HUD, found by itself (`fg_hud.h`, shaders in `hud.hlsl`; off unless
`Hud=1` or an area asks for it; docs/FRAMEGEN-LEARNINGS.md, 14):

- A HUD does not move, and a generator moves what it sees: icons and digits
  over moving scenery come out faded or doubled. An area left alone (`Keep`)
  cures that with the nearer real frame, for everything in the area - the
  scenery seen beside and through the HUD too. The mask is that cure for the
  HUD's own pixels only, and needs no area.
- Once per pair of real frames, two counters per pixel go one step on: a
  pixel that stayed as it was (nothing changing within 2 px) while the
  scenery's motion says it should have changed, and an edge that stayed the
  same edge across a motion that would have moved it, count up; a pixel that
  changes lets go at once. The motion is the block motion of step 2 from a to
  b - the pixel's own cell's, and the background's around it (per block of
  8x8 cells the median of the cells that move). On from two pairs; the mask
  is that, grown by 2 px.
- After the frame in between is made - by the app's own generator or by DLSS
  Frame Generation - one pass draws the nearer real frame where the mask is
  set: over the whole picture with `Hud=1`, else inside the areas marked
  `/hud`. Areas without the mark are copied whole, as before.
- A generator carries what it sees along the scenery's motion, HUD pixels
  too: a faint copy of an icon half a step from the icon. With `HudClean`
  (the app's own generator) the mask goes its pair on as soon as the draw has
  found the motion (`fg::Interpolator::after_motion`), and the frame is drawn
  from the two real frames without the HUD the mask holds
  (`fg::HudMask::clean_frames`): under each HUD pixel of one frame, what the
  scenery shows there in the other frame a step along the motion, or - where
  that is HUD as well, or nothing moves - the earlier frame's scenery from
  the nearest place the HUD ends. The pass above then puts the HUD back.
  Copies of HUD parts the mask does not hold stay.
- DLSS Frame Generation hands out no motion: the pyramids and the search one
  way (steps 1 and 2) are run for the mask alone
  (`fg::Interpolator::find_flow`). A picture too small to search gets a rule
  without motion (detail that does not change while the frame as a whole
  does, over four pairs). With `Method=blend` nothing is looked for: a mix
  moves nothing.
- The counters start over when the picture's size or place changes, on a
  switch between SDR and HDR, and after a second without a pair (frame
  generation off or paused, a still picture). A cut is not told to them: the
  pixels that change are let go by the rule itself.
- Thresholds are steps of 8-bit SDR luma; for HDR10 (PQ) a step counts half
  as much of the code range. That is derived, not measured on HDR captures.
  Not run on an scRGB back buffer.
- Cost on an RTX 5090: 0.24-0.32 ms once per real frame at 5120x1440 and
  3840x2160 (about twice that with the search for DLSS Frame Generation), and
  0.02-0.03 ms per frame in between. `ReShade.log` says what it works on
  ("HUD mask: on, everywhere, 3840x2160, SDR levels, by the generator's
  motion") and, with the 5 s line, how much the mask holds and what it costs.

Frames that do not just move:

- Fades and flashes: every comparison is made after correcting for how much
  the picture brightened or darkened - first estimated from the coarse
  pyramid, then, once A's motion is known, from content that actually
  matches (so a pan bringing in brighter scenery is not taken for a fade).
- Cuts: when more than a fifth of the frame in between matched nothing, or
  more than 40 % of its cells with detail did, the frame in between repeats A
  instead of mixing two scenes. (Flat areas match under any motion, even
  across a cut: a cut between two shots with much dark red sky in common left
  only 20.6 % of all cells unmatched, but 54 % of those with detail; the
  busiest real pair in the captures, a fast spear fight, reaches 34 %.)
- A window smaller than about 64 pixels gets the 50/50 blend; generation
  comes back at the next size.

About 2.2 ms on the RTX 5070 Ti at 3440x1440 (1.2 of it the per-pixel step,
0.5 the search both ways); the wrong-frame check beside slow things adds up
to about 0.3 ms during fast camera turns and nothing in a pan, the character
layer (step 6) about 0.07 ms in a pan and 0.15-0.3 ms with a character
(measured 2.9-3.3 ms per generated frame on a busy GPU, 2026-10-29). A cap on
the mix's softness where a both-frames option already fits (`cap_fit`,
`cap_cell`, off by default) thinned the faint second edge beside a still head
in fast orbits a little, but also wiped the faint ghost of a fast spear head
(20261025-000605-950) that a viewer reads as the spear - off.

## Measured (synthetic source through the real chain, 165 Hz display)

| Source | Window | Result after DLSS 5 warm-up |
| --- | --- | --- |
| 30 fps (as 60 with repeats) | 1280x720 | 29.9 real + 29.9 generated, 0 dropped, real frame within 0.011 ms avg |
| 30 fps (as 60 with repeats) | full screen 3440x1440 | 29.9 real + 29.9 generated, 0 dropped |
| 60 fps | full screen 3440x1440 | 60 real + ~58 generated, 0 dropped, ~2/s skipped (queue full) |

The first seconds after start drop frames with or without frame generation
(DLSS 5 starting up); the add-on waits them out and starts on its own, with
no back-off. With frame generation off, mpv drops the same frames there.
With `Method=motion` at full screen (3440x1440), 30 fps runs at 30 + 30 with
the real frame on time. 60 fps is at the edge of what the GPU can do next to
the DLSS 5 pass (about 9.5 ms of every 16.7): one run held 60 real + 55-58
generated with the real frame about 2.5 ms late, later runs dropped a few
source frames, and the add-on then backed off and generated almost nothing.
Screenshots with `DebugMarker=1` showed generated frames on screen, sharp,
without the blend's double image. All of this used a synthetic source through
`Lerad --input-raw`; the capture card has not been run with this
folder yet.

On screen with VRR (`vrr-test.ps1 -Steady`, PresentMon, 2026-10-30), how long
each frame stayed up:

| Source | Real frame timed from mpv's present (before) | Held until half an interval after the frame in between (now) |
| --- | --- | --- |
| 30 fps | generated 15.0 ms, real 18.3 ms (stdev 2.2) | generated 16.67 +- 0.39, real 16.67 +- 1.00 (stdev 0.76) |
| 60 fps | generated 5.2-5.4 ms, real 11.3-11.5 ms (stdev 3.1) | generated 8.34 +- 0.27, real 8.33 +- 0.47 (stdev 0.38) |

Before, the frame in between reached the screen about 7 ms after mpv's
present (the DLSS 5 pass and its drawing) while the real frame went out 8.3
ms after it: at 60 fps the two came almost together. Without frame
generation a 30 fps source stays up 33.33 +- 0.94 ms.

The hold can only space the frames evenly while the frame in between is done
well within half an interval; otherwise it lets the real frame go just before
the next frame's effects (counted as "let go early" in the log), so as not to
hold up its DLSS 5 pass. With NVIDIA's "Prefer maximum performance" for mpv
the frame in between took about 2 ms longer at 60 fps (GPU time per frame
10.2 ms instead of 8.2; 7.6 instead of 6.2 without frame generation), and
60 fps gave 6.2 / 10.5 ms; back at the default power setting it is 8.34 +-
0.41 / 8.33 +- 0.56 ms again (old timing 5.20 / 11.47). 30 fps is
16.66 / 16.68 either way. With DLSS 5's neural rendering on
(`NeuralUplift=1` in `[RenoDX.DLSS5]`) the DLSS 5 pass alone takes about 15.7
ms per frame: at 30 fps the frame in between is done about 16 ms after the
add-on drew it and the frames stay up 16.63 +- 0.63 / 16.70 +- 0.75 ms; at 60
fps there is no room for it and frame generation backs off by itself (60
real frames).

## Quality (offline, `eval\fg_eval.exe`)

PSNR of the frame in between against the true one, on clips made from game
screenshots (higher is better; "repeat" shows the earlier frame again):

| Clip | repeat | blend | motion |
| --- | --- | --- | --- |
| pan_slow (12 px/frame) | 22.6 | 25.2 | 60.9 |
| pan_med (40 px/frame) | 18.2 | 20.6 | 49.9 |
| pan_fast (96 px/frame) | 15.5 | 17.5 | 37.5 |
| pan_diag | 18.8 | 21.2 | 47.5 |
| zoom | 18.2 | 20.7 | 37.0 |
| sprite (object over moving background) | 17.6 | 20.3 | 35.4 |
| orbit (camera turning around a still character) | 17.3 | 19.7 | 33.6 |
| orbit_drift (the same, character drifting) | 16.7 | 19.3 | 29.6 |
| hud (still HUD over a pan) | 18.8 | 21.2 | 39.7 |
| noisy_hud (still HUD, grain on every frame) | 22.6 | 25.0 | 34.4 |
| subs (still subtitles over a pan) | 18.3 | 20.7 | 40.6 |
| ui_fade (HUD fading in over a pan) | 18.6 | 21.1 | 36.1 |
| ui_slide (menu panel sliding in) | 24.8 | 26.7 | 33.8 |
| menu_scroll (text) | 22.2 | 24.4 | 54.8 |
| pan_1440 (2560x1440) | 19.7 | 22.2 | 54.7 |
| fade_static (fade to black) | 36.3 | 57.1 | 57.1 |
| fade_pan (fade while panning) | 18.8 | 21.4 | 49.1 |
| cut (one pair across a hard cut) | 17.7 | 20.8 | 40.9 |
| **mean** | 20.2 | 23.6 | **42.9** |

`Quality=fast` (2x2 cells, one pixel compared) is cheaper and scores lower
(41.2 mean).

The scores above 45 dB are all invisible; the fade-outs at the cell edges
(step 5) and the softer mix where nothing fits cost a little there and gain on
the hard clips (sprite, subs, UI, orbits) and on game captures. Trying no
motion in the search (step 2) gains on still things over a pan (subs +2.0)
and on the game captures, and costs a little on the pans.

(At the cut the frame in between is A again; no true in-between frame exists
there, so that clip's score is mostly its three ordinary pairs.)

What is left shows beside the edges of moving things: in the strip one frame
cannot see (scenery a turning camera uncovers behind a character), the
motion is known per 16x16 pixels and the pixels drawn there are a little off.

## Captures

The capture key (F9) saves what frame generation worked with at that moment
into `Captures\<date-time>\` next to mpv.exe: five real frames in a
row as the DLSS 5 pass left them (`f01.png` - `f05.png`, 16-bit for HDR, 8-bit
for SDR - also when mpv draws SDR into a 10-bit back buffer, as it does on a
10-bit desktop, so 16-bit always means PQ to the viewer and the eval tools) and
the frames the add-on drew between them (`between_f01_f02.png`, ...). Press it
while something looks wrong (frame generation must be on); there is one short
hitch while it saves, and a toast says when the files are written (100-150 MB
per capture). The bug report made with them (`Bug reports\`, a few MB) holds
JPEG pictures of these frames, HDR tone-mapped to SDR, not the frames: for
measuring, ask for the folder in `Captures\` that its system.txt names.

`tools\view_captures.cmd [FOLDER]` (double-click it for the captures in
`out\Lerad\Captures`; it starts `src\framegen\eval\view_captures.py` and
needs Python with Pillow, and ffmpeg on PATH) shows them: pick a capture on the left, then
Left/Right steps through f01, the generated frame, f02, ... tone-mapped like
`eval\compare.sh`. Zoom (mouse wheel, `1` = 100 %) and pan stay put while
stepping, so one spot can be compared frame to frame; Space plays the capture
in a loop (1x = 60 frames a second, as on screen, down to 1/30x); `R` skips
the generated frames for comparison; Up/Down changes capture; the status bar
gives the frame pixel under the cursor. Delete (or the Delete button) moves the
capture on show to the Recycle Bin and opens the next one, so a run of
captures can be pruned key by key; Shift+Delete deletes it for good, after
asking. An unpacked bug report can be given as the folder: its pictures are
shown the same way.

`eval\fg_eval.exe Captures` scores such folders like its own clips:
it makes the frame between f01 and f03 and compares it with f02 (and f03-f05
with f04) - twice the motion of real use, on the game's own pictures. On a
synthetic pan through the real chain: motion 38.2 dB, blend 23.0, repeat 20.6.
On sixteen captures of a game (2026-10-25/26: combat with effects, running,
camera turns and orbits around a standing or running character): motion
31.7 dB, blend 28.1, repeat 26.2. Side-by-side reviews of the frames the add-on draws in them (the frame
between each two in a row), the build before against the new one: for the
search around no motion and the cut check (44 pairs), 13 better - the faint
copies of hair and face beside a head during camera turns gone - 31 the
same, none worse except a small red dot on a tilting minimap fading for one
frame; for the persistence (all 64 pairs, 2026-10-26), 24 better - the
ground showing through the top of the head during camera turns gone, HUD
labels over moving scenery solid - 40 the same, none worse; the restored
spikes can be a little ragged at their tips; for the wrong-frame check
beside slow things (22 captures, 88 pairs, 2026-10-28), the faint extra hair
tips and second head outlines in fast orbits gone, also doubled NPCs,
dialogue lines and minimap edges; a version that also widened "still" to
anything much slower than the scenery tore holes in walls and a walking
NPC's dress, and was dropped. A spike that turns while the head drifts can
still come out soft or cut at its tip. Fast swings and limbs are as
soft as before, and the cut between two shots repeats the earlier frame
exactly. Blend comes close because PSNR favours blur where nothing
can be followed (an attack, lightning); there motion gives a blur-like smear
as well.

`eval\blockiness.py FRAME.png ...` measures how much more the picture jumps
across the 16-pixel cell edges than elsewhere (1.0 = no more). The game's own
frames in those captures: 1.08 mean, 1.22 worst. The frames the add-on drew
in the first eight, before step 5: 1.18 mean, 1.55 worst. All 64 pairs now
(`fg_eval.exe --adjacent`): 1.04 mean, 1.13 worst.

## Build and test

- `build.cmd` (repo root) builds the add-on with the rest of Lerad and
  installs it into `out\Lerad\engine`; the shaders are compiled with fxc
  into headers (`shaders.cmake` lists them). `Lerad.exe --setup` puts
  ReShade (add-on build, from reshade.me) next to mpv.exe.
- Settings: `lerad.ini` `[framegen]` (on/off, quality, method, multiplier,
  max_output_hz, every frame while off). Lerad writes them into
  `engine\ReShade.ini`, section `[LERAD_FRAMEGEN]`, before mpv starts; the
  other keys there (DebugMarker, ResetMs, ToggleKey, CaptureKey, ...) can be
  edited by hand. The add-on logs to `engine\ReShade.log`.
- The arithmetic of the pacing and of the menu's numbers is in `fg_pacing.h`
  (no Direct3D), tested by `tests\test_fg_pacing.cpp` with the menu's wording
  (`src\overlay\framegen_text.h`).
- Without a card: `python tools\smoke_raw.py --seconds 12` runs the whole chain
  (ffmpeg test pattern -> `Lerad.exe --input-raw -` with dedup -> mpv ->
  ReShade -> the add-on); its 5 s stats lines are in ReShade.log. With
  `LERAD_FRAMEGEN_TRACE=<file.csv>` the add-on writes one line per real
  frame (when it arrived, mpv's present, the frame in between done on the
  GPU and the real frame let go, in ms after the add-on ran).
  `LERAD_FRAMEGEN_CAPTURE_AFTER=<s>` takes a capture, and
  `LERAD_FRAMEGEN_TOGGLE_AFTER=<s>` presses F8 once, that many seconds in.
- `eval\fg_eval.exe` (built as `build\vs\src\framegen\Release\fg_eval.exe`)
  scores repeat, blend and motion with the same `fg_interp.h` the add-on runs:
  `fg_eval.exe [--clip NAME] [--out DIR] [--profile] [--stats] [--set NAME VALUE] SEQ_DIR`
  on the clips `eval\make_sequences.sh` makes from game screenshots, or on
  capture folders. The clips and captures are game pictures and stay out of
  the repository. (`--stats` prints each pair's brightness change and
  unmatched share; `--adjacent` makes the frames between each two in a row of
  a capture, as the add-on did, and saves them without a score; `--set NAME
  VALUE` changes one of `fg_interp.h`'s settings; `--set debug_view 1` colours
  each pixel by the option used: blue both frames, green B alone, red A alone,
  brighter for more motion; 2 shows the cost the softness follows, red where
  it may go wide; 3 how badly each cell's motion matched; 4 how far the motion
  found there goes - red A's blocks, green B's, blue the frame in between's,
  white 128 pixels and up; 5 the costs at each pixel, x2 - red standing still
  from both frames, green the best motion from both, blue the best from one
  frame alone.) A change to the shaders or `fg_interp.h` should keep or
  raise these scores; the port to Glasir gave the prototype's numbers
  exactly (synthetic mean 42.93 dB).
- The HUD mask is scored along longer clips, as it needs a few pairs of
  history: `fg_eval.exe --hud BOXES.json [--methods motion,dlss] [--hud-rules
  motion,plain] [--hud-set NAME VALUE] [--hud-save 8,12 --out DIR] SEQ_DIR`,
  on clips of 33 frames in a row (pair k is frames 2k-1 and 2k+1, the true
  frame 2k) with the HUD's boxes marked by hand in BOXES.json (`{"clip":
  [{"name": "minimap", "box": [left, top, width, height]}, ...]}`). Every
  pair is run in order with `fg_hud.h` as the add-on runs it, and each frame
  in between is scored as it is, with the boxes copied whole, with the mask
  inside the boxes and with the mask everywhere: the error on the HUD's own
  pixels, on the rest of the boxes and outside them, the whole frame's PSNR,
  how much of the HUD the mask holds pair by pair, and the GPU time of the
  detector and of the mask's pass. `--resize W H` scales the frames first
  (the times at another size); `--as-pq NITS` gives them to the generator and
  the mask as HDR10 (PQ). A change to `hud.hlsl` or `fg_hud.h` should be
  judged on all the clips, and `fg_eval` without `--hud` must score what it
  did before.
- `eval\compare.sh OUT.png W:H:X:Y SCALE A.png B.png ...` puts tone-mapped
  crops of HDR frames side by side; `eval\blockiness.py --diff A.png B.png`
  lists where two versions of a frame differ most; `eval\view_captures.py`
  shows captures (Tk, Pillow, ffmpeg on PATH); `eval\needle_score.py` and
  `eval\head_score.py` score thin hair and heads on captures (`FG_CAP`);
  `eval\hud_score.py` scores what stands still on the screen while the scene
  moves (a HUD), on `fg_eval --adjacent` frames.
- `eval\raw_pace.exe` sends raw frames at an even 1/60 s like a card (ffmpeg
  `-re` jumps 4-43 ms between frames, and with VRR the screen shows that).
- Not ported yet: the prototype's VRR measurement under PresentMon
  (`vrr-test.ps1`, `vrr_report.py`). The pacing numbers below come from it.

## Limits of this version

- Motion is known per 16x16 pixels: edges of moving objects can show small
  errors where one frame hides what the other shows.
- What no motion search can follow (lightning, sparks, a very fast attack) is
  drawn as a soft smear, not as it would look half way.
- The HUD mask (`Hud=1`, areas with `/hud`) holds the HUD's own pixels: the
  faint copy the generator draws beside a HUD part goes only where the mask
  holds that part (`HudClean`; text that does not stand out from what is
  behind it is not held), a HUD part that moves or
  fades is let go while it does, and something that stands still on screen
  over moving scenery (a cockpit frame) is taken for a HUD. Scored on SDR
  clips of three games; its HDR thresholds are derived, not measured.
- x2 by default; x3 and x4 are an option (`Multiplier`), checked for timing
  and for where the frames in between sit (a moving bar at 0.24 / 0.50 / 0.76
  and 0.34 / 0.66 of its step, from F9 captures of a synthetic source), not
  for picture quality on game captures: `fg_eval` scores only the middle
  frame, and thirds and quarters draw more from one of the two frames.
- A multiplier that fills the screen's rate (30 fps x4 on 120 Hz) has no
  spare refresh. In a window Windows composes it is stepped down to x3 when
  game frames keep going without their frames ("A multiplier that cannot be
  held"); one game frame's frames left out every few seconds is under that
  rule's limit and stays as it is. The rule's numbers come from one PC, one
  screen and a synthetic 30 fps source.
- At 60 fps full screen the GPU is near its limit next to the DLSS 5 pass (see
  Measured): earlier runs backed off; on 2026-10-30 synthetic runs held 60 + 60
  with no drops.
- Latency: the real frame is shown half a source frame after the frame in
  between, which waits for the DLSS 5 pass and its own drawing. Measured
  from mpv's present to the screen with VRR (2026-10-30): without frame
  generation 6.7 ms at 30 fps and 5.7 ms at 60; with it the frame in between
  shows at 8.5 / 7.5 ms (its drawing, +1.8) and the real frame at 25.2 / 15.9
  ms - 18.5 ms later at 30 fps, 10.2 ms at 60 (half a source frame plus the
  drawing).
- Even pacing needs VRR (G-SYNC) with mpv in the foreground; at a fixed 165 Hz
  frames land on whole refreshes (e.g. 6 and 12 ms at 60 fps). See "What the
  screen does to the pacing".
- The hold needs Direct3D 11.4 fences and a Direct3D 12 device on the same GPU
  (ReShade wraps it like any other; the DLSS 5 add-ons logged nothing about
  it in the tests). Without
  them the real frame is timed from mpv's present as before (5 / 11.5 ms on
  screen at 60 fps).
- Every present counts as a real frame, including mpv redrawing the same video
  frame for its OSD; the frame between such a redraw and the next frame is the
  picture on screen anyway.
