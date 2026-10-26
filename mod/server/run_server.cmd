@echo off
rem Starts the local CSNZ_Server (127.0.0.1:30002, SSL on). The runtime DLLs sit next to the exe,
rem so no MinGW/WinLibs folder is needed on PATH. Output goes to server.log next to this script.
rem stdin is kept open on purpose: the server spins at 100% CPU when its console input hits EOF.
cd /d "%~dp0CSNZ_Server\bin"
powershell -NoProfile -Command "Start-Sleep -Seconds 999999" | "%~dp0CSNZ_Server\bin\CSNZ_Server.exe" > "%~dp0server.log" 2>&1
