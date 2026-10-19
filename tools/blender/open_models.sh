#!/bin/sh
# Opens Deadground's models in Blender (docs/ITEM_PIPELINE.md "Open models in Blender"), from Git Bash / a shell:
#   tools/blender/open_models.sh rifle             tools/blender/open_models.sh all
#   tools/blender/open_models.sh --list            tools/blender/open_models.sh all --kind weapon,attachment
# Every argument goes to open_models.py. Python: the py launcher (Windows), else python3, else python.
here=$(cd "$(dirname "$0")" && pwd)
if command -v py >/dev/null 2>&1; then
    exec py -3 "$here/open_models.py" "$@"
fi
p3=$(command -v python3 2>/dev/null)
case "$p3" in
    ""|*WindowsApps*) ;;   # the Microsoft Store stub opens the store instead of running
    *) exec python3 "$here/open_models.py" "$@" ;;
esac
if command -v python >/dev/null 2>&1; then
    exec python "$here/open_models.py" "$@"
fi
echo "open_models: no Python 3 found" >&2
exit 1
