# EZ2.4G — C 版构建脚本
# 用法：在 c\ 目录运行 build.bat
@echo off
setlocal

set GCC="D:\Program Files (x86)\Dev-Cpp\MinGW64\bin\gcc.exe"
set WINDRES="D:\Program Files (x86)\Dev-Cpp\MinGW64\bin\windres.exe"

set SRC=src\hid.c src\config.c src\protocol.c src\usbpcap.c src\round.c src\icon.c src\ui.c src\log.c src\main.c
set OUT=build\EZ2.4G.exe

if not exist build mkdir build

echo [1/2] 编译资源...
%WINDRES% res\app.rc -O coff -o build\app.res
if errorlevel 1 goto fail

echo [2/2] 链接...
%GCC% -municode -O2 -s -Wall -Wextra -DUNICODE -D_UNICODE ^
  %SRC% build\app.res -o %OUT% ^
  -luser32 -lgdi32 -lshell32 -lsetupapi -lhid -lcomctl32 -ladvapi32 -lole32 -lshlwapi -lmsimg32 -lgdiplus
if errorlevel 1 goto fail

echo.
echo 构建成功: %OUT%
dir %OUT% | findstr EZ2.4G
exit /b 0

:fail
echo.
echo 构建失败
exit /b 1
