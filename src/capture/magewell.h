// SPDX-License-Identifier: MIT
// Magewell Pro Capture cards: their driver's low latency mode.
//
// Normally the card hands a frame to the PC once all of it has come in. In
// low latency mode it passes lines on while the frame is still arriving (the
// "Low latency mode" box of the driver's own panel). Measured on a Pro
// Capture HDMI 4K Plus at 3840x2160 P010 59.94 (11-17): frames ready at the
// same point of every frame and 16.6-16.8 ms apart, against 2.4 and 7.4 ms
// later in two runs and 15.5-18.0 ms apart without it.
//
// Against the card's own clock (the MWCapture SDK telling when each frame was
// whole in the card's buffer, Media Foundation alone capturing; same card and
// signal, 11-17): with the mode a sample reaches the program 1.1-1.3 ms after
// its frame's last line (the driver's stamp 0.2-0.4 ms after it), 16.4-17.0
// ms apart; without it the driver copies on its own timer, anywhere in the
// frame from run to run. The SDK's own low latency capture has the frame in
// memory as the last line arrives, the same for 64, 128 and 256 lines, and
// its whole-frame capture 14 ms later: a millisecond to gain over this mode,
// for a vendor DLL and a second capture path. Not taken.
//
// The mode is one byte of the driver's video settings, a kernel streaming
// property on the card's video capture interface (which a Media Foundation
// device's symbolic link names): property set
// {3A45B930-FF5C-42AA-A5A2-43771B93EBEF}, property 0x2F for streams of the
// capture pin and 0x35 for the preview pin, 49 bytes each (the MWCapture
// SDK's MWCAP_VIDEO_PROCESS_SETTINGS, packed), the flag in byte 28. It
// applies to streams opened afterwards, and the driver keeps it in its
// registry key, so it also holds for other programs.
#pragma once
#include <string>

namespace app {

// Whether the capture device is a Magewell card (PCI vendor 1CD7), from its
// symbolic link.
bool is_magewell(const std::wstring &symbolic_link);

// Reads the mode. False when the driver does not answer.
bool magewell_low_latency(const std::wstring &symbolic_link, bool &on);

// Switches the mode for streams opened from now on. False when the driver
// does not take it.
bool set_magewell_low_latency(const std::wstring &symbolic_link, bool on);

}  // namespace app
