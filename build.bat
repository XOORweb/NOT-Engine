@echo off
tcc -shared Engine.c -o NOT_Engine.dll
tcc Program.c NOT_Engine.dll -o NOT_enhost.exe
del /q NOT_Engine.def

echo 1 ICON "NE-Logo.ico" > temp_icon.rc

ResourceHacker.exe -open temp_icon.rc -save temp_icon.res -action compile
ResourceHacker.exe -open NOT_enhost.exe -save NOT_enhost.exe -action addoverwrite -res temp_icon.res

del /q temp_icon.rc
del /q temp_icon.res 2>nul
pause