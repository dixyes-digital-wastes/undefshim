@echo off
REM What the image's own startnet.cmd runs, if it finds one on a drive.
REM
REM The demos are beside this script, but a call by name from the drive's own
REM directory leaves %~dp0 empty, and a path built from that is relative to
REM the current drive -- an X: ramdisk on this image. So the drive is looked
REM for, with this script's own directory preferred when it has one.

setlocal

set "here=%~dp0"
if not "%here%"=="" if exist "%here%lrcpc_demo.exe" goto :run

for %%d in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist "%%d:\lrcpc_demo.exe" (
        set "here=%%d:\"
        goto :run
    )
)

echo entrypoint: no demos on any drive
echo entrypoint: done
exit /b 1

:run
REM The exit status is printed because the screen is the only output there is
echo entrypoint: running from %here%

"%here%lrcpc_demo.exe"
echo entrypoint: lrcpc_demo exit %errorlevel%

"%here%udf_demo.exe"
echo entrypoint: udf_demo exit %errorlevel%

echo entrypoint: done
