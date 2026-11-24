-- SPDX-License-Identifier: MIT
-- The s key sends "script-message app-screenshot". The app's overlay (the
-- ReShade add-on) takes the screenshot: the picture on screen, after scaling
-- and DLSS 5 (src/overlay/screenshot.h), and says so in its toast. It sets
-- user-data/app/screenshot when it connects; without it (no ReShade, or
-- it has not started yet), mpv saves the video frame itself.
local mp = require 'mp'

mp.register_script_message('app-screenshot', function()
    if mp.get_property_native('user-data/app/screenshot') then return end
    mp.command('no-osd screenshot video')
    mp.commandv('script-message', 'app-toast', 'Screenshot saved')
end)
