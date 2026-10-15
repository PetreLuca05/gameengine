#!/bin/sh
# Headless-ish GPU VFX test: runs tools/tests/vfx_gpu.cfg (>300 frames) with the Vulkan
# validation layers and fails if any validation message was printed.
cd "$(dirname "$0")/../../build/bin" || exit 2
LOG=../vfx_gpu_test.log
VK_ADD_LAYER_PATH="${VK_ADD_LAYER_PATH:-C:\C\Game\.toolchain\vvl\install\bin}" ./deadground.exe --validation --test --nosound --script ../../tools/tests/vfx_gpu.cfg > "$LOG" 2>&1
rc=$?
errs=$(grep -c "\[vk\]" "$LOG")
frames=$(grep -c "" "$LOG")
grep "vfx" "$LOG" | tail -5
echo "exit=$rc validation_messages=$errs"
[ "$rc" -eq 0 ] && [ "$errs" -eq 0 ]
