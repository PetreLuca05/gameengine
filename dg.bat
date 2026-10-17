@echo off
rem Short launcher for the openable build:  dg [n^|c] [map]
rem   dg            asks nanite / classic
rem   dg n          nanite        dg c   classic
rem   dg n vegetation   straight into a map (no splash / menu); "dg n handpose" opens the editor
set R=%~1
if /i "%R%"=="n" set R=nanite
if /i "%R%"=="c" set R=classic
set P="%~dp0openable builds\Deadground\Play.bat"
if "%~2"=="" (call %P% %R%& goto :eof)
if /i "%~2"=="handpose" (call %P% %R% --nomenu +scene handpose& goto :eof)
call %P% %R% --nomenu +map %~2
