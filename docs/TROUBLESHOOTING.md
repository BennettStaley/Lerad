<!-- SPDX-License-Identifier: MIT -->
# Troubleshooting

If none of this helps, press **F9** while the problem is happening and attach
the zip from `Bug reports\` to an
[issue](https://github.com/BennettStaley/Lerad/issues) (see the README's
"Something wrong?").

## What should the console be set to?

Lerad's window says it under **Console, set it to**, for the capture mode
and signal you chose (rest the mouse on an entry for the reason), and
`Lerad.exe --system` prints the same with the reasons. The console cannot
work it out: it only sees the capture card.

- **Resolution**: the size Lerad captures (2160p for a 3840x2160 mode).
- **HDR**: on for an HDR capture, off for an SDR one. The wrong way round
  looks grey and washed out, or far too dark.
- **120 Hz output**: off, unless you chose a 120 fps capture mode.
- **VRR**: off. The card needs a steady rate. Variable refresh is used on
  the PC's screen, where frame generation shows each frame at its moment.
- **HDR calibration**: when Lerad sends HDR on as the console sends it (the
  default), the window also shows "Calibrate HDR once": run the console's
  HDR calibration once while playing through Lerad, so it is set for the
  screen you play on. With "Fit highlights" on (Picture tab), Lerad does
  the fitting and the hint is not shown.

## The window offers less than I expected

- **x3 or x4 is greyed out** and says "does not fit" ("Up to", on the Frame
  generation tab): it would add no more frames on this screen. On a screen
  without variable refresh at 120 Hz, a 30 fps game at x3 would be 90 fps,
  which does not fall on whole refreshes, so x2 is used; x4 (120 fps) does.
  A 60 Hz screen has room for x2 of a 30 fps game only. When such a choice
  was already set, the line under "Up to" says why it changes nothing.
- **HDR is greyed out** under "Signal" on the Play tab: the card has no
  10-bit mode.
- **The HDR settings on the Picture tab are greyed out**: "Signal" is set
  to SDR (Play tab).
- **The screen shows "variable refresh on or off: not known"**: with an AMD
  or Intel GPU Lerad can read what the screen can do, not whether it is
  switched on. Say so under "Variable refresh" on the Screen tab.
- **A screen is missing or shows the wrong HDR or variable refresh state**:
  the window reads the screens again when you come back to it from Windows'
  or the GPU's settings. `Lerad.exe --system` says how each fact was found.

## Frame generation does not double the frame rate

Open the menu (`Tab`). Its top shows what the card delivers and what is
shown, measured over the last second (`30 → 60 fps`), and under it the
picture's size, SDR or HDR, and the screen (`120 Hz VRR`). With frame
generation off it shows the game's rate alone. "Show frame rates" on the
Frames tab keeps the rates and the screen in the top left corner while you
play.

Under that, the menu says what is in the way when fewer frames are shown
than asked for: a quiet line, or a warning with a button where the menu can
do something about it.

- **"Paused, back in N s"**, "The GPU could not keep up": mpv dropped
  frames, so frame generation stepped back for a while (5 s, then longer if
  it keeps happening). DLSS 5 and frame generation share the GPU; at
  60 → 120 fps each frame has about 8 ms. Press **Use Fast quality**, or
  choose a smaller "Up to", a lighter DLSS 5 setting (or DLSS 5 off), or
  capture at a lower resolution. Close other programs that use the GPU.
- **"No frames added: 60 fps doubled is more than this 100 Hz screen
  shows"**: twice the game's rate would be more than the screen can show
  (60 fps needs a 120 Hz screen).
- **"x2, not x4: more would be over what this 120 Hz screen shows"**: you
  asked for up to x4, and x2 is the most that fits this game's rate.
- **"... over the limit of 100 fps"**: the same two, when "Limit" on the
  Frame generation tab of Lerad's window is what stops it.
- **"x3, not x4"**, "The screen takes frames late at x4": x4 fits the
  screen, but frames waited and some game frames went without their added
  frames. Lerad then shows one fewer for every game frame, so the rhythm
  stays even (30 fps at 90, not 120). It tries x4 again by itself, after
  20 seconds and then less often, and at once when you change a setting or
  the window. Press **Switch to full screen**: a window is where this
  mostly happens. With "The GPU cannot keep up at x4", press **Use Fast
  quality** or switch DLSS 5 off.
- **"104 of 120 fps"**, "The screen takes frames late": Windows is composing
  Lerad's window instead of showing it directly, so frames wait and some
  are left out. Press **Switch to full screen**. If it stays, switch "Skip
  black bars" off on the Screen tab of Lerad's window, and click into the
  picture.
- **"104 of 120 fps"**, "... of the game's frames were dropped": the GPU is
  at its limit; as for "Paused" above.
- **"The picture is still: nothing to add"**: a paused game or a still menu
  sends no new frames.
- The "Frame generation" switch is greyed out: the card runs in basic mode
  (a card Windows' own video capture cannot open), which has no frame
  generation.

The choices on the menu's Frames tab:

| Choice | What it gives | What it costs |
|---|---|---|
| Quality **High** / **Fast** | Fast: about half the GPU time of each added frame | Rougher edges around moving things |
| Method **Motion** / **Blend** | Blend: the lightest; a plain mix of two frames | Moving things show double |
| Up to **x2** / **x3** / **x4** | x3, x4: two or three frames between the game's (30 → 90 or 120 fps) on a screen fast enough | GPU time for each added frame, and the game's own frame is shown later: half a game frame at x2, two thirds at x3, three quarters at x4 (17, 22 and 25 ms at 30 fps) |

Lerad uses the most frames that fit the screen, up to your choice: with x4
on a 120 Hz screen, a 30 fps game is shown at 120 and a 60 fps game at 120
(x2). "Limit" on the Frame generation tab of Lerad's window sets a lower
ceiling: 100 fps on a 120 Hz screen with variable refresh keeps a 30 fps
game at 90 (x3), inside the screen's range.

In the NVIDIA Control Panel, leave "Power management mode" for mpv at its
default: "Prefer maximum performance" made each frame about 2 ms slower in
tests.

## Frames look uneven (judder) with frame generation

How evenly the added frames reach the screen depends on the screen. The menu
says when yours makes them uneven.

- **A VRR screen (G-SYNC, FreeSync), the result below its refresh rate**
  (30 → 60 or 90 on 120 Hz): even. Lerad shows each frame at its moment and
  the screen follows.
- **A VRR screen, the result equal to its refresh rate** (60 → 120, or
  30 → 120 with x4, on 120 Hz): "120 fps fills this 120 Hz screen: variable
  refresh has no room left, so frames follow its refreshes." That is usually
  still smooth; if it is not, choose x3 for a 30 fps game (90 fps, inside the
  range).
- **"Variable refresh is not engaging"**, "The screen keeps refreshing at
  120 Hz.": Lerad watched the screen refresh at its full fixed rate while
  showing fewer frames, so VRR is not working for Lerad's window. See
  "VRR / G-SYNC" below. Until it does, frames land on whole refreshes and
  can alternate short and long.
- **A screen without VRR whose rate is a multiple of the result** (60 or
  120 fps on 120 or 240 Hz): frames land on whole refreshes; mostly even,
  with an occasional short and long pair. Measured on a 120 Hz screen
  without VRR at 3840x2160: 30 → 60 fps held two refreshes for about nine
  frames in ten, and 30 → 120 fps (x4) one refresh for about nine in ten,
  with around 103 of the 120 frames shown, as nothing is left over when the
  result fills the screen's rate.
- **A screen without VRR whose rate is not a multiple** (120 fps on 144 or
  165 Hz): **"Uneven on this screen"**, "120 fps does not divide 144 Hz, so
  some frames stay longer than others." Set the screen to the rate the menu
  names (120 Hz) in Windows' display settings, or turn variable refresh on.

## Text or icons of the HUD smear or show double in the added frames

An added frame follows what moves. A HUD does not move with the scenery
behind it, so its text and icons can smear or show double.

- Switch **Find the HUD** on: on the Frame generation tab of Lerad's
  window, or in the menu (Frames tab, **Edit** beside "Areas left alone",
  then "Find the HUD everywhere"). Added frames then show the HUD as the
  game drew it. It finds most of a HUD within a few frames and all of it
  within about a second; in the first moments after a HUD appears, or
  after the scene changes, a copy of it can still show beside it.
- **Numbers that change every moment** (a counter, a compass tape) are not
  held by Find the HUD and can still show double. Draw an area over them.
- **A map or subtitles**: draw an area over them. In the menu, **Edit**
  beside "Areas left alone", then **Rectangle** or **Ellipse**, and drag
  over the picture. Leave the area as "Whole area": the added frames show
  the game's own frame there.
- **Ghost copies beside a HUD in very fast motion**: draw an area over the
  HUD, or choose NVIDIA DLSS Frame Generation under "Frames by" (NVIDIA
  RTX 40 or 50 series).

## VRR / G-SYNC

- `Lerad.exe --system` shows what Lerad found for each screen: whether it
  has variable refresh, its range, whether it is switched on, and how each
  fact was found (the monitor's EDID, NVIDIA's driver). The log
  (`logs\lerad.log`) has the same at every start.
  - **"VRR off ... not offered on this screen"** (NVIDIA): the monitor can,
    but G-SYNC is not set up for it. In the NVIDIA Control Panel, "Set up
    G-SYNC", pick that screen and tick "Enable settings for the selected
    display model" (monitors NVIDIA has not tested are off until you do),
    and check that Adaptive-Sync / FreeSync is on in the monitor's own menu.
  - **"VRR supported"**: the monitor can, but Lerad cannot read whether it
    is switched on. That is all it can tell with an AMD or Intel GPU today.
    If FreeSync is on in the GPU's software, set "Variable refresh" to On
    on the Screen tab of Lerad's window.
  - **"VRR unknown"**: nothing could be read (some monitors with a G-SYNC
    module, or a screen on an adapter). Set "Variable refresh" to On or
    Off there.
- With more than one screen, "Play on" (Play tab or Screen tab) picks the
  one Lerad plays on; variable refresh, HDR and the refresh rate are then
  read for that screen.
- In the NVIDIA Control Panel, G-SYNC must be on **for windowed and full
  screen mode** (Lerad's full screen and borderless modes are both windows
  that cover the screen; neither is exclusive full screen).
- **"The screen would keep its fixed rate: the NVIDIA driver's profile ...
  allows G-SYNC for mpv ..."** under "Worth a look" in Lerad's window: the
  driver has a setting for mpv that says less than the global one. **Fix**
  allows G-SYNC for Lerad's own mpv alone.
- G-SYNC follows the focused window: clicking a window on another screen
  turns VRR off for Lerad until you click back into it.
- Lerad's own full screen covers the whole screen, which keeps VRR. A
  window smaller than the screen ("Window", or "Borderless" with "Skip
  black bars") keeps it only while Windows can give it a hardware overlay
  plane (MPO), which other running apps can take away; it is then composed
  by Windows, a frame later, and frame generation reaches only about 100 of
  120 fps. If the menu shows less than double, switch "Skip black bars" off
  on the Screen tab of Lerad's window.
- To check: with frame generation on, the menu warns "Variable refresh is
  not engaging" when the screen keeps its fixed rate. The screen's own
  refresh rate display (most have one in their menu) should follow the rate
  Lerad shows.
- If the menu shows the wrong thing after "Hz" at its top ("VRR", "fixed",
  or nothing when it could not tell), set "Variable refresh" to On or Off
  on the Screen tab of Lerad's window. The menu's Screen tab says which one
  frame generation goes by.

## Other frame generation: NVIDIA Smooth Motion, AMD AFMF, Lossless Scaling

Lerad's own frame generation is the one to use: it works on any GPU, sees
only the game's new frames, runs after DLSS 5, keeps the menu out of the
generated frames and times them for VRR. Use one frame generator at a time:
turn Lerad's off (F8) before trying another.

- **NVIDIA Smooth Motion** (RTX 40 and 50, set per program in the NVIDIA
  app): x2 only. The one report with mpv found it did nothing on Direct3D 11,
  which Lerad uses. Not recommended.
- **AMD Fluid Motion Frames** (Radeon RX 6000 and newer): needs full screen
  or borderless with VSync off, switches itself off in fast motion, and AMD
  recommends a base of 55 fps or more, so a 30 fps game is below it. It also
  treats Lerad's menu as part of the picture.
- **Lossless Scaling** (a paid program, any GPU): works on Lerad's window
  as on any other, up to x20, and can run on a second GPU. It captures the
  finished window, so the menu and messages are interpolated too, and it
  adds its own delay. The choice if you have a second GPU to give it.
- Intel has no driver frame generation for programs that do not build it in.

## Black bars on an ultrawide screen

A 16:9 console on a 21:9 screen is shown as large as fits, with black bars
at the sides. DLSS 5 works on the whole screen, bars included (on 3440x1440
about 9.7 ms a frame instead of 5.5 for the picture alone); at 60 fps with
frame generation that can leave too little room, and frame generation then
pauses on its own. Neither the DLSS 5 add-on nor its feeder can be limited
to a part of the screen, but Lerad's window can: "Skip black bars" on the
Screen tab of Lerad's window (for "Borderless") makes it the picture's
size, black around it (about 5.5 ms instead of 9.7). Check the fps in the
menu (Tab) afterwards: where Windows composes such a window, frame
generation falls short of double, and the option is better off.

## HDR

- HDR needs HDR on the console and one of the card's 10-bit modes. "Capture
  mode" in Lerad's window lists the card's 10-bit modes when "Signal" is
  Automatic or HDR. With SDR it lists the others, and marks with "HDR" the
  ones the card also has in 10-bit.
- Cards often offer HDR only at some sizes and rates; the Live Gamer Ultra
  2.1, for example, has 4K HDR only at 30 fps but 1440p HDR at 60. Automatic
  uses those modes too: for an SDR console at 4K 60, choose SDR.
- With **Signal: Automatic** Lerad tells SDR from HDR by the picture and
  follows the console while you play (about a second for SDR, five for
  HDR; a black screen says nothing, so it waits for a picture). The top of
  the menu says what it is showing: `SDR`, `HDR`, or `HDR to SDR` for an
  HDR signal that is tone mapped. With "Signal" on the menu's Picture tab
  you can say SDR or HDR yourself. A 10-bit capture switches at once; an
  8-bit one starts the picture again. The first switch at a window size
  can stop the picture for some seconds while ReShade prepares its effects
  for the other kind.
- **The colours are wrong with Automatic** (washed out and dim: HDR shown as
  SDR; glaring: SDR shown as HDR): set "Signal" to SDR or HDR yourself, in
  the menu or Lerad's window. It can happen with a console whose HDR peak is
  set to 5000 nits or more (very bright scenes then read as SDR), and with a
  grey, dim SDR game that shows no white for five seconds (read as HDR).
  `logs\lerad.log` has a "signal:" line whenever what it sees changes; a
  bug report (F9) with it helps.
- The menu's Picture tab, under "HDR", says what is done with the HDR
  picture:
  - **"HDR10 to the screen, as the console sends it"**: Windows has HDR on
    for the screen (Settings, System, Display, "Use HDR"), and the picture
    goes out untouched.
  - **"Tone mapped to SDR"**, with the reason: the screen has no HDR, or
    HDR is off in Windows. The picture is brought down to SDR so that it
    looks right, without HDR's highlights. Turn "Use HDR" on in Windows and
    start the picture again to get HDR. "Send it as" on the Picture tab of
    Lerad's window ("Always HDR" or "Always SDR") decides it yourself, if
    Lerad reads the screen wrong.
- **Highlights clip to white, or the picture is too dim or too bright.** Set
  the console's HDR calibration (PS5: Settings, Screen and Video, Adjust
  HDR; Switch 2 and Xbox have the same) while playing through Lerad, as
  you would on a TV: the console then sends what this screen can show. Set
  "Console's peak" on the Picture tab of Lerad's window to the peak you
  ended up with if you know it.
- **A console that cannot be calibrated to the screen**: switch "Fit
  highlights to this screen" on in the menu (Picture tab). Lerad then brings
  highlights brighter than the screen down to its peak and leaves the rest
  as it is. "Screen peak" starts at what the screen reports to Windows,
  which is often too low (a 1000 nit screen reporting 400): raise it until
  bright highlights stop gaining detail. "Measure each frame's peak" suits
  games that never reach the peak they announce, for some GPU time. The
  Picture tab of Lerad's window has the same: "Fit highlights", "Screen
  peak", "Curve" and "Measure each frame".
- **The menu is too bright or too dim in HDR.** Its white follows Windows'
  "SDR content brightness" for the screen (Settings, System, Display, HDR);
  "Menu brightness" on the Picture tab of Lerad's window sets it yourself.
- **An SDR game with HDR on in Windows** is shown like any SDR window, at
  Windows' "SDR content brightness".

## The picture is soft or scaled

Lerad scales the card's picture to its place on the screen before DLSS 5.
A capture mode of your screen's height (2560x1440 on a 1440p screen) needs no
scaling; a 4K capture is scaled down anyway.

For a smaller capture, try "Scaling" on the menu's Picture tab: **FSR 1**
gives sharper edges on any GPU; **VSR** (NVIDIA RTX, Intel) uses the GPU's
video engine and cleans up compression too; **ArtCNN** rebuilds the most
detail on any GPU; **Anime4K** suits drawn and cel-shaded games. VSR,
ArtCNN and Anime4K cost GPU time, which DLSS 5 and frame generation also
need; if frame generation starts pausing, go back to FSR 1. If VSR cannot
run (the driver turns it down), Lerad says so and uses FSR 1.

## The capture card does not open

Another program may be using it: OBS, Discord, a browser tab, or the card's
own app. Close it and start Lerad again. `Lerad.exe --list` shows the cards
Lerad finds and their modes.

## No sound, or it is out of sync

- Lerad plays the card's own audio input; `logs\audio.log` says which one it
  found. `Ctrl+R` restarts the sound after changing the output device.
- `[` / `]` hold the sound back or bring it forward (10 ms a press), to match
  the picture; frame generation and DLSS 5 delay the picture a little.

## DLSS 5 is not available

DLSS 5 needs an NVIDIA RTX 30, 40 or 50 series GPU and its download: the
**Download** button beside "DLSS 5" in Lerad's window (Play tab or About
tab), or `Lerad.exe --setup`.

The first time Lerad plays after DLSS 5 is installed or updated, the
picture can stand still for about 20 seconds, and DLSS 5 shows up after
about 40: ReShade compiles DLSS 5's shader (with its motion estimator) and
keeps the result. It does so once for each picture size and for SDR and
HDR, so the first switch to the other one, or to another window size, waits
the same way once. Later starts are quick again.

On RTX 30 and 40 series GPUs DLSS 5 takes more GPU time than on an RTX 50
(about 1.5 times as much on an RTX 4070 SUPER as on an RTX 5070 Ti). If the
picture stutters with it on, switch it off on the menu's Picture tab or on
the Play tab of Lerad's window; both keep it off. F6 turns it off until
Lerad closes.

DLSS 5 is judged for the GPU that draws (**GPU** on the Screen tab of
Lerad's window). On a laptop whose screen is on the integrated GPU, a note
at the top of the Screen tab offers the RTX GPU, with what that costs: each
frame is then copied between the two.

## Lerad closed by itself

Only one Lerad runs at a time: starting another one closes the first.
Otherwise `logs\lerad.log` says why it ended.
