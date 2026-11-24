@echo off
rem SPDX-License-Identifier: MIT
rem Opens the viewer for the frame-generation captures (F9 while playing):
rem each capture in the built app's Captures folder (in out\) in order, real and
rem generated frames, tone-mapped for the screen. Needs Python with Pillow, and
rem ffmpeg on PATH.
rem   tools\view_captures.cmd            the captures of the built app
rem   tools\view_captures.cmd FOLDER     another folder of captures
start "" pythonw "%~dp0..\src\framegen\eval\view_captures.py" %*
