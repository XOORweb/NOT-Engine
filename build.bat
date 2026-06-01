@echo off
.lib\tcc -shared Engine.c -o NOT_Engine.dll
.lib\tcc Program.c NOT_Engine.dll -o NOT_enhost.exe
del /q NOT_Engine.def
.lib\icon-changer.exe NE-Logo.ico NOT_enhost.exe

pause