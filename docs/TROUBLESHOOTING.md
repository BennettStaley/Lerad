<!-- SPDX-License-Identifier: MIT -->
# Troubleshooting

If none of this helps, press **F9** while the problem is happening and attach
the zip from `Bug reports\` to an
[issue](https://github.com/Abhito/Glasir/issues) (see the README's
"Something wrong?").

## What should the console be set to?

Glasir's window says it under **Set your console to**, for the capture mode
and picture you chose, and `Glasir.exe --system` prints the same with the
reasons. The console cannot work it out: it only sees the capture card.

- **Resolution**: the size Glasir captures (2160p for a 3840x2160 mode).
- **HDR**: on for an HDR capture, off for an SDR one. The wrong way round
  looks grey and washed out, or far too dark.
- **120 Hz output**: off, unless you chose a 120 fps capture mode.
- **VRR**: off. The card needs a steady rate. VRR is used on the PC's
  screen, where frame generation shows each frame at its moment.
- **HDR calibration**: when Glasir sends HDR on as the console sends it (the
  default), run the console's HDR calibration once while playing through
  Glasir, so it is set for the screen you play on. With "Fit highlights to
  this screen" on, Glasir does the fitting and the hint is not shown.

## The window offers less than I expected

- **x3 or x4 is greyed out**: the line under "Up to" on the Frame generation
  tab says why. On a screen without VRR at 120 Hz, a 30 fps game at x3 would
  be 90 fps, which does not fall on whole refreshes, so x2 is used; x4
  (120 fps) does. A 60 Hz screen has room for x2 of a 30 fps game only.
- **HDR is greyed out**: the card has no 10-bit mode.
- **The HDR tab is greyed out**: the capture is SDR (Picture, on the Play
  tab).
- **VRR shows "on or off: not known"**: with an AMD or Intel GPU Glasir can
  read what the screen can do, not whether it is switched on. Say so under
  "VRR is" on the Frame generation tab.
- **A screen is missing or shows the wrong HDR or VRR state**: the window
  reads the screens again when you come back to it from Windows' or the
  GPU's settings. `Glasir.exe --system` says how each fact was found.

## Frame generation does not double the frame rate

Open the menu (`Tab`). Its top line shows what the card delivers, what is
shown and the screen, measured over the last second:
`30 → 60 fps · 120 Hz VRR`. With frame generation off it shows the game's
rate alone. "Show frame rates" keeps the same line in the corner while you
play.

Under "Frame generation" the menu says what is in the way when fewer frames
are shown than asked for:

- **"Paused, back in N s: the GPU could not keep up"**: mpv dropped frames, so
  frame generation stepped back for a while (5 s, then longer if it keeps
  happening). DLSS 5 and frame generation share the GPU; at 60 → 120 fps each
  frame has about 8 ms. Choose **Fast** quality, a smaller multiplier, a
  lighter DLSS 5 setting, or capture at a lower resolution. Close other
  programs that use the GPU.
- **"No frames added: 60 fps doubled is more than this 100 Hz screen
  shows"**: twice the game's rate would be more than the screen can show
  (60 fps needs a 120 Hz screen).
- **"x2, not x4: more would be over what this 120 Hz screen shows"**: you
  asked for up to x4, and x2 is the most that fits this game's rate.
- **"104 of 120 fps: the screen takes frames late"**: Windows is composing
  Glasir's window instead of showing it directly, so frames wait and some
  are left out. Use full screen or borderless, with "Skip the black bars"
  off, and click into Glasir.
- **"... of the game's frames were dropped"**: the GPU is at its limit; as
  for "Paused" above.
- **"The picture is still: nothing to add"**: a paused game or a still menu
  sends no new frames.
- The checkbox is greyed out: the card is used through DirectShow (cards
  Media Foundation cannot open), which has no frame generation.

The choices under "Frame generation":

| Choice | What it gives | What it costs |
|---|---|---|
| Quality **High** / **Fast** | Fast: about half the GPU time of each added frame | Rougher edges around moving things |
| Method **Motion** / **Blend** | Blend: the lightest; a plain mix of two frames | Moving things show double |
| Up to **x2** / **x3** / **x4** | x3, x4: two or three frames between the game's (30 → 90 or 120 fps) on a screen fast enough | GPU time for each added frame, and the game's own frame is shown later: half a game frame at x2, two thirds at x3, three quarters at x4 (17, 22 and 25 ms at 30 fps) |

Glasir uses the most frames that fit the screen, up to your choice: with x4
on a 120 Hz screen, a 30 fps game is shown at 120 and a 60 fps game at 120
(x2). `[framegen] max_output_hz` in glasir.ini sets a lower ceiling: 100 on a
120 Hz VRR screen keeps a 30 fps game at 90 (x3), inside the VRR range.

In the NVIDIA Control Panel, leave "Power management mode" for mpv at its
default: "Prefer maximum performance" made each frame about 2 ms slower in
tests.

## Frames look uneven (judder) with frame generation

How evenly the added frames reach the screen depends on the screen. The menu
says when yours makes them uneven.

- **A VRR screen (G-SYNC, FreeSync), the result below its refresh rate**
  (30 → 60 or 90 on 120 Hz): even. Glasir shows each frame at its moment and
  the screen follows.
- **A VRR screen, the result equal to its refresh rate** (60 → 120, or
  30 → 120 with x4, on 120 Hz): "120 fps fills this 120 Hz screen". VRR has
  no room left, so frames follow the screen's refreshes. That is usually
  still smooth; if it is not, choose x3 for a 30 fps game (90 fps, inside the
  range).
- **"VRR is not engaging: the screen keeps refreshing at 120 Hz"**: Glasir
  watched the screen refresh at its full fixed rate while showing fewer
  frames, so VRR is not working for Glasir's window. See "VRR / G-SYNC"
  below. Until it does, frames land on whole refreshes and can alternate
  short and long.
- **A screen without VRR whose rate is a multiple of the result** (60 or
  120 fps on 120 or 240 Hz): frames land on whole refreshes; mostly even,
  with an occasional short and long pair. Measured on a 120 Hz screen
  without VRR at 3840x2160: 30 → 60 fps held two refreshes for about nine
  frames in ten, and 30 → 120 fps (x4) one refresh for about nine in ten,
  with around 103 of the 120 frames shown, as nothing is left over when the
  result fills the screen's rate.
- **A screen without VRR whose rate is not a multiple** (120 fps on 144 or
  165 Hz): "Uneven on this screen: 120 fps does not divide 144 Hz". Set the
  screen to the rate the menu names (120 Hz) in Windows' display settings,
  or turn VRR on.

## VRR / G-SYNC

- `Glasir.exe --system` shows what Glasir found for each screen: whether it
  has variable refresh, its range, whether it is switched on, and how each
  fact was found (the monitor's EDID, NVIDIA's driver). The log
  (`logs\glasir.log`) has the same at every start.
  - **"VRR off ... not offered on this screen"** (NVIDIA): the monitor can,
    but G-SYNC is not set up for it. In the NVIDIA Control Panel, "Set up
    G-SYNC", pick that screen and tick "Enable settings for the selected
    display model" (monitors NVIDIA has not tested are off until you do),
    and check that Adaptive-Sync / FreeSync is on in the monitor's own menu.
  - **"VRR supported"**: the monitor can, but Glasir cannot read whether it
    is switched on. That is all it can tell with an AMD or Intel GPU today.
    If FreeSync is on in the GPU's software, set `[display] vrr = on` in
    `glasir.ini`.
  - **"VRR unknown"**: nothing could be read (some monitors with a G-SYNC
    module, or a screen on an adapter). Set `[display] vrr = on` or `off`.
- With more than one screen, `[display] monitor` picks the one Glasir plays
  on (its number in `--system`, or a part of its name); VRR, HDR and the
  refresh rate are then read for that screen.
- In the NVIDIA Control Panel, G-SYNC must be on **for windowed and full
  screen mode** (Glasir's full screen and borderless modes are both windows
  that cover the screen; neither is exclusive full screen).
- G-SYNC follows the focused window: clicking a window on another screen
  turns VRR off for Glasir until you click back into it.
- Glasir's own full screen covers the whole screen, which keeps VRR. A
  window smaller than the screen (a window, or borderless with "Skip the
  black bars") keeps it only while Windows can give it a hardware overlay
  plane (MPO), which other running apps can take away; it is then composed
  by Windows, a frame later, and frame generation reaches only about 100 of
  120 fps. If the menu shows less than double, turn "Skip the black bars"
  off.
- To check: with frame generation on, the menu warns "VRR is not engaging"
  when the screen keeps its fixed rate. The screen's own refresh rate
  display (most have one in their menu) should follow the rate Glasir shows.
- If Glasir shows the wrong thing after "Hz" in the menu's top line ("VRR",
  "fixed" or nothing when it could not tell), set `[display] vrr = on` or
  `off` in glasir.ini.

## Other frame generation: NVIDIA Smooth Motion, AMD AFMF, Lossless Scaling

Glasir's own frame generation is the one to use: it works on any GPU, sees
only the game's new frames, runs after DLSS 5, keeps the menu out of the
generated frames and times them for VRR. Use one frame generator at a time:
turn Glasir's off (F8) before trying another.

- **NVIDIA Smooth Motion** (RTX 40 and 50, set per program in the NVIDIA
  app): x2 only. The one report with mpv found it did nothing on Direct3D 11,
  which Glasir uses. Not recommended.
- **AMD Fluid Motion Frames** (Radeon RX 6000 and newer): needs full screen
  or borderless with VSync off, switches itself off in fast motion, and AMD
  recommends a base of 55 fps or more, so a 30 fps game is below it. It also
  treats Glasir's menu as part of the picture.
- **Lossless Scaling** (a paid program, any GPU): works on Glasir's window
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
to a part of the screen, but Glasir's window can: "Skip the black bars" in
Glasir's window (`[window] skip_black_bars = 1`) makes it the picture's
size, black around it (about 5.5 ms instead of 9.7). Check the fps in the
menu (Tab) afterwards: where Windows composes such a window, frame
generation falls short of double, and the option is better off.

## HDR

- HDR needs HDR on the console and one of the card's 10-bit modes. Glasir's
  window lists the card's HDR modes when HDR is chosen, and marks them with
  "HDR" in SDR.
- Cards often offer HDR only at some sizes and rates; the Live Gamer Ultra
  2.1, for example, has 4K HDR only at 30 fps but 1440p HDR at 60.
- Switch between SDR and HDR in the menu; the picture starts again.
- The menu (Tab), under Display, says what is done with the HDR picture:
  - **"HDR10 to the screen, as the console sends it"**: Windows has HDR on
    for the screen (Settings, System, Display, "Use HDR"), and the picture
    goes out untouched.
  - **"HDR tone mapped to SDR"**, with the reason: the screen has no HDR, or
    HDR is off in Windows. The picture is brought down to SDR so that it
    looks right, without HDR's highlights. Turn "Use HDR" on in Windows and
    start the picture again to get HDR. `[hdr] output = hdr` or `sdr` in
    glasir.ini decides it yourself, if Glasir reads the screen wrong.
- **Highlights clip to white, or the picture is too dim or too bright.** Set
  the console's HDR calibration (PS5: Settings, Screen and Video, Adjust
  HDR; Switch 2 and Xbox have the same) while playing through Glasir, as
  you would on a TV: the console then sends what this screen can show. Set
  `[video] hdr_peak_nits` in glasir.ini to the peak you ended up with if
  you know it.
- **A console that cannot be calibrated to the screen**: tick "Fit highlights
  to this screen" in the menu. Glasir then brings highlights brighter than
  the screen down to its peak and leaves the rest as it is. "Screen peak"
  starts at what the screen reports to Windows, which is often too low (a
  1000 nit screen reporting 400): raise it until bright highlights stop
  gaining detail. "Measure each frame's peak" suits games that never reach
  the peak they announce, for some GPU time. These are `[hdr]
  fit_to_screen`, `display_peak_nits`, `tone_mapping` and `dynamic_peak` in
  glasir.ini.
- **The menu is too bright or too dim in HDR.** Its white follows Windows'
  "SDR content brightness" for the screen (Settings, System, Display, HDR);
  `[hdr] paper_white_nits` sets it yourself.
- **An SDR game with HDR on in Windows** is shown like any SDR window, at
  Windows' "SDR content brightness".

## The picture is soft or scaled

Glasir scales the card's picture to its place on the screen before DLSS 5.
A capture mode of your screen's height (2560x1440 on a 1440p screen) needs no
scaling; a 4K capture is scaled down anyway.

For a smaller capture, try "Scaling" in the menu: **FSR 1** gives sharper
edges on any GPU; **VSR** (NVIDIA RTX, Intel) uses the GPU's video engine
and cleans up compression too; **ArtCNN** rebuilds the most detail on any
GPU; **Anime4K** suits drawn and cel-shaded games. VSR, ArtCNN and Anime4K
cost GPU time, which DLSS 5 and frame generation also need; if frame
generation starts pausing, go back to FSR 1. If VSR cannot run (the driver
turns it down), Glasir says so and uses FSR 1.

## The capture card does not open

Another program may be using it: OBS, Discord, a browser tab, or the card's
own app. Close it and start Glasir again. `Glasir.exe --list` shows the cards
Glasir finds and their modes.

## No sound, or it is out of sync

- Glasir plays the card's own audio input; `logs\audio.log` says which one it
  found. `Ctrl+R` restarts the sound after changing the output device.
- `[` / `]` hold the sound back or bring it forward (10 ms a press), to match
  the picture; frame generation and DLSS 5 delay the picture a little.

## DLSS 5 is not available

DLSS 5 needs an NVIDIA RTX 30, 40 or 50 series GPU and its download: the **Update**
button in Glasir's window, or `Glasir.exe --setup`.

On RTX 30 and 40 series GPUs DLSS 5 takes more GPU time than on an RTX 50
(about 1.5 times as much on an RTX 4070 SUPER as on an RTX 5070 Ti). If the
picture stutters with it on, turn it off in the menu (Tab) or with the
checkbox in Glasir's window; both keep it off. F6 turns it off until Glasir
closes.

DLSS 5 is judged for the GPU that draws (**GPU** in Glasir's window). On a
laptop whose screen is on the integrated GPU, the window offers the RTX GPU
under Screen, with what that costs: each frame is then copied between the
two.

## Glasir closed by itself

Only one Glasir runs at a time: starting another one closes the first.
Otherwise `logs\glasir.log` says why it ended.
