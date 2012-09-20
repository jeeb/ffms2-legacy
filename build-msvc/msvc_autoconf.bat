@echo off
rem	This here thingie is sort of a ghetto autoconf.
rem	Its main purpose is to find out if you built ffmpeg with certain additional libraries or not;
rem	if it fails to do so properly you may override its decisions in src/config/config.h.
rem	Note: it most likely won't work outside MSVC since it needs the $LIB environment variable.

setlocal
set "configfile=../src/config/auto_config.h"
rem Truncate the auto_config.h file
echo /* auto-generated by msvc_autoconf.bat */ > "%configfile%"

rem	Find out where the mingw libraries reside.
rem	%%~dp$LIB:F means "Search the directories listed in the LIB environment variable for the
rem	filename stored in %%F; then expand to the full path name of the directory where the first
rem	such file was found."
rem	For some reason Microsoft in their infinite wisdom chose to make this useful functionality
rem	a variable name expansion instead of (god forbid) a general file finder program or something.
rem	Please kill me now.
for %%F in ("libavformat.a") do set mingwlibpath=%%~dp$LIB:F

if not exist "%mingwlibpath%libavformat.a" goto error
if exist "%mingwlibpath%libopencore-amrnb.a" echo #define WITH_OPENCORE_AMR_NB >> %configfile%
if exist "%mingwlibpath%libopencore-amrwb.a" echo #define WITH_OPENCORE_AMR_WB >> %configfile%
if exist "%mingwlibpath%libpthreadGC2.a" echo #define WITH_PTHREAD_GC2 >> %configfile%

goto :EOF


:error
echo Library autodetection failed. If you get unresolved externals,
echo you may have to edit src/config/msvc-config.h manually.
