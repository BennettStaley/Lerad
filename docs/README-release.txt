Lerad - modern PC graphics for game consoles
https://github.com/BennettStaley/Lerad

GETTING STARTED

1. Unzip this folder somewhere you can write to, for example
   C:\Games\Lerad. Not "Program Files": Lerad keeps its settings, logs
   and screenshots in its own folder.
2. Plug in your capture card and turn the console on.
3. Start Lerad.exe. On the Play tab of its window, pick your card, the
   signal (Automatic, SDR or HDR) and the screen to play on. "What you
   will get" on the right says what your choices come to on this PC, and
   "Console, set it to" what to set on the console for them (resolution,
   HDR, 120 Hz output, VRR off). The Picture, Frame generation, Screen,
   Sound and About tabs hold the rest.
4. Press Play. The first time, it asks to download the programs it runs on
   (mpv and ReShade, plus DLSS 5 on an NVIDIA RTX 30, 40 or 50 series GPU).
   They come straight from their authors' sites, in the versions this
   Lerad was tested with; THIRD-PARTY-NOTICES.md lists them and their
   licences.
5. While playing, Tab opens the menu. SDR or HDR is found by itself
   ("Signal: Automatic"); the menu and Lerad's window can also say which.

"Lerad.exe --system" prints what Lerad found on the PC and what your
settings will do there.

Settings are in lerad.ini (written on the first start, every setting
explained). Volume, boost and audio delay you change while playing are kept
there, and so is what you change in the menu (Tab).

KEYS

  Tab                      the menu (Esc or Tab closes it): DLSS 5, frame
                           generation, scaling, sound, full screen, SDR,
                           HDR or automatic
  double-click, f          full screen
  Up / Down, mouse wheel   volume
  m                        mute
  Ctrl+Up / Ctrl+Down      audio boost (up to 400 %)
  [ / ]                    audio delay -/+ 10 ms (lip sync)
  Ctrl+r                   restart the sound
  F6                       DLSS 5 on/off
  F8                       frame generation on/off
  F9                       save a bug report, with the frames on screen
  s                        screenshot (in Screenshots\)
  q, Esc                   quit (Esc closes the menu first if it is open)
  Home                     ReShade's overlay (DLSS 5 and effect settings)

DLSS 5 starts with its authors' default settings. To tune it, open
ReShade's overlay (Home): RenoDX-DLSS5's settings are on the Add-ons tab;
DLSS5_Feed's on the Home tab (untick "Performance Mode" there to edit them).
Your changes are kept in engine\ReShade.ini and engine\ReShadePreset.ini.

UPDATING

Lerad does not look for newer versions of itself. To move to a newer one,
unpack it over this folder (your settings stay).

  About tab (the window)   says when this Lerad was tested with other
                           versions of mpv, ReShade and DLSS 5 than yours;
                           "Download tested versions" there gets them
  Lerad.exe --setup        the same, from a command line

REPORTING A PROBLEM

Press F9 while it happens: a zip with the logs, settings, your GPUs and
screens (and pictures of the frames on screen, with frame generation on) is
saved in Bug reports\. It is a few MB and your Windows user name is left out.
Attach it to an issue at https://github.com/BennettStaley/Lerad/issues and
say what you saw. The frames themselves are saved in Captures\ (about
100 MB each time): keep the folder the report names in case they are asked
for, and delete the others whenever you like. "Save a bug report" on the
About tab saves one without frames.

To remove Lerad, delete its folder.

Logs: logs\lerad.log, logs\audio.log, engine\ReShade.log.
