-- SPDX-License-Identifier: MIT
-- Tells the app when mpv leaves its own full screen (f, a double-click, the
-- menu), so the app puts the borderless window back in place: mpv would
-- restore the size it had before, which is not the picture's size when
-- full screen skips the black bars ([window] skip_black_bars).
local mp = require 'mp'

local first = true
mp.observe_property('fullscreen', 'bool', function(_, on)
    if first then
        first = false  -- the value at start, not a change
        return
    end
    if on == false then mp.commandv('script-message', 'app-window', 'again') end
end)
