# SPDX-License-Identifier: MIT
"""The product's name for the Python tools, read from the one place it is set:
the project() line of the root CMakeLists.txt. What the app builds from it
(cmake/app_version.h.in) is built the same way here, so no tool types it.
"""
import os
import re

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def _project_name():
    with open(os.path.join(REPO, 'CMakeLists.txt'), encoding='utf-8') as f:
        found = re.search(r'^project\(\s*(\w+)', f.read(), re.MULTILINE)
    if not found:
        raise SystemExit('No project() name in the root CMakeLists.txt.')
    return found.group(1)


NAME = _project_name()            # as people read it
ID = NAME.lower()                 # the settings file, the log, mpv's IPC pipe
EXE = NAME + '.exe'
INI = ID + '.ini'
LOG = ID + '.log'                 # in the app folder's logs
PIPE = '\\\\.\\pipe\\' + ID
ENV_PREFIX = NAME.upper() + '_'   # the environment variables
APP_DIR = os.path.join(REPO, 'out', NAME)  # where build.cmd puts the app
