@echo off
rem Short launcher for the openable build:  dg [map]
rem   dg              main menu
rem   dg vegetation   straight into a map (no splash / menu); "dg handpose" opens the editor
rem   dg n ... / dg c ...   the old renderer picks still work (ignored: the classic renderer is the only one)
set M=%~1
if /i "%M%"=="n" set M=%~2
if /i "%M%"=="c" set M=%~2
if /i "%M%"=="nanite" set M=%~2
if /i "%M%"=="classic" set M=%~2
set P="%~dp0openable builds\Deadground\Play.bat"
if "%M%"=="" (call %P%& goto :eof)
if /i "%M%"=="handpose" (call %P% --nomenu +scene handpose& goto :eof)
call %P% --nomenu +map %M%
