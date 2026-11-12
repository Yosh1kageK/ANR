@echo off
rem Removes nr-record's capture DLL from a game: uninstall.cmd "<folder of the game's .exe>"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
