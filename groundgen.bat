@echo off
rem Ground textures (docs/GROUND_REFERENCE.md): the one command for a machine with the Higgsfield key in .env.
rem Generates every missing image of assets/foliage/gen + assets/terrain/gen (about $10 the first time, cached),
rem processes them, checks / re-rolls failing blade cells, writes docs/ground_match/gen_report.md, then commits
rem only the generated paths and pushes the current branch.
rem   groundgen                 everything
rem   groundgen --dry-run       list the requests + cost, call nothing
rem   groundgen grass_broad     some recipes (names = file names in the gen folders)
rem   groundgen --no-push ...   keep it local (commit only)
set PUSH=--push
if /i "%~1"=="--no-push" (set PUSH=--commit& shift)
where py >nul 2>nul && (set PY=py) || (set PY=python)
%PY% -c "import numpy, PIL" 2>nul || %PY% -m pip install --user numpy pillow
%PY% "%~dp0tools\vfxgen\groundgen.py" %PUSH% %1 %2 %3 %4 %5 %6 %7 %8 %9
