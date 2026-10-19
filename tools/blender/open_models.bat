@echo off
rem Opens Deadground's models in Blender (docs/ITEM_PIPELINE.md "Open models in Blender"), from cmd or PowerShell:
rem   tools\blender\open_models.bat rifle            tools\blender\open_models.bat all
rem   tools\blender\open_models.bat --list           tools\blender\open_models.bat all --kind weapon,attachment
rem Every argument goes to open_models.py. Python: the py launcher, else python on PATH, else Blender's own.
setlocal
set "OM=%~dp0open_models.py"
where py >nul 2>nul || goto no_py
py -3 "%OM%" %*
exit /b %errorlevel%
:no_py
where python >nul 2>nul || goto no_python
python "%OM%" %*
exit /b %errorlevel%
:no_python
for /d %%B in ("%ProgramFiles%\Blender Foundation\Blender *") do for /d %%V in ("%%B\*") do if exist "%%V\python\bin\python.exe" set "BPY=%%V\python\bin\python.exe"
if not defined BPY goto none
"%BPY%" "%OM%" %*
exit /b %errorlevel%
:none
echo open_models: no Python 3 found (install it, or Blender, which brings its own)
exit /b 1
