# Contributing to Lerad

Thanks for helping. A few rules keep the licensing clean.

## Licences

- Everything is **MIT**, except the frame generation code in
  `src/framegen/`, which is **MPL-2.0**.
- Every source file starts with its licence tag, in the file's comment style:

  ```cpp
  // SPDX-License-Identifier: MIT
  ```
  ```cpp
  // SPDX-License-Identifier: MPL-2.0
  ```

- Changes to MPL-2.0 files stay MPL-2.0. Don't move frame generation code
  into MIT files to get around that, and don't copy code from other
  projects unless its licence allows it; say where it came from.

## Sign your commits (DCO)

By contributing you certify the [Developer Certificate of Origin](https://developercertificate.org/):
that you wrote the change or have the right to submit it under the
project's licences. Add a sign-off line to every commit:

```
git commit -s -m "Describe the change"
```

which adds `Signed-off-by: Your Name <you@example.com>`.

## What doesn't go in the repo

- Third-party binaries (mpv, ffmpeg, ReShade, DLSS DLLs, add-ons). The
  app downloads them on the user's PC (`src/app/components.cpp`).
- Game footage or screenshots. Evaluation tools read captures from a local
  folder outside the repo.
- Third-party source with an unclear licence.

## Building and testing

`build.cmd` needs Visual Studio 2022 or 2026 with "Desktop development with
C++" (it finds Visual Studio and its CMake by itself):

```
build.cmd            build, test, put the app in out\Lerad
build.cmd check      build and test only (out\ is not touched)
build.cmd package    ... and the release zip, out\Lerad-x.y.z.zip
```

GitHub Actions runs the same on every push and pull request
(`.github/workflows/build.yml`); a tag `v1.2.3` also opens a draft release
with the zip. Third-party programs are not in the repo:

```
out\Lerad\Lerad.exe --setup --no-dialogs   # mpv and ReShade into out\Lerad\engine
out\Lerad\Lerad.exe --list                 # the capture cards and their modes
```

`--setup --no-dialogs --components mpv,reshade,dlss5` picks what to
download. Setup installs the versions in `src/app/components.json` (built
into `Lerad.exe`); `--setup --newest` takes the newest of each instead, and
`python tools\pin_components.py --engine COPY\engine` writes a tried set
back into that file.

Without a capture card (Python; `smoke_raw.py` also needs ffmpeg):

```
python tools\smoke_raw.py                    # NV12, a 30 fps game captured at 60
python tools\smoke_raw.py --format p010 --hdr
python tools\hdr_check.py                    # HDR end to end (needs numpy)
python tools\probe.py --set video.mode=hdr   # with a real card, in a small window
```

Run `build.cmd` before sending a change. Changes to capture,
frame generation or pacing should also be tried with a real capture card
when possible; say in the pull request what you tested on (GPU, card,
console).

## Layout

```
src/capture/    finding cards, reading frames, dedup (library)  MIT
src/app/        Lerad.exe: settings, starting mpv, frame pump  MIT
src/common/     shared with the add-ons (link, INI, JSON)       MIT
src/overlay/    in-game menu, built into the frame gen add-on   MIT
src/audio/      WASAPI audio mpv plugin                        MIT
src/framegen/   frame generation ReShade add-on + shaders      MPL-2.0
tools/          developer scripts (fetch mpv, smoke tests)     MIT
third_party/    vendored headers with their licences           (see there)
tests/          build and unit tests                           MIT
docs/           plan, guides, troubleshooting                  MIT
```
