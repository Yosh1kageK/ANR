@echo off
rem Installs nr-record's capture DLL into a game that has OptiScaler: install.cmd "<folder of the game's .exe>"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
