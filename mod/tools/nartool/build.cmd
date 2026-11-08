@echo off
rem builds nartool.exe (x86) with its version information; needs Visual Studio's C++ tools
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
rc /nologo nartool.rc || exit /b 1
cl /nologo /O2 /EHsc nartool.cpp nartool.res || exit /b 1
del nartool.obj nartool.res 2>nul
