@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Pack Crimson Text into a tar.gz archive in dist
setlocal
cd /d "%~dp0.."

call "%~dp0build.cmd"
if errorlevel 1 exit /b %errorlevel%
set "PROGRAM=crimson-text"
set "APPLICATION_ID=io.github.CrimsonOrg.CrimsonText"
set "VERSION=0.1.0"
set "PREFIX=%HOME%/.local"
set "STD=c11"
set "WARN=-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
set "OPT=-O2 -g"
set "PKGS=gtk4 libadwaita-1 gtksourceview-5 libcurl glycin-2"
set "LIBS=-lm"
set "DEFINES=-DAPPLICATION_VERSION=%VERSION%"
set "INCLUDES=-Isrc -Isubprojects/crimson-core/src"
set "EXTRA_LICENSES=md4c=src/md4c/LICENSE"
echo(==^> sh subprojects/crimson-core/scripts/package.sh
sh subprojects/crimson-core/scripts/package.sh
if errorlevel 1 exit /b %errorlevel%
exit /b 0
