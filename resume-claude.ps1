# 1. LIPIȘTE PROMPTUL TĂU ÎNTRE GHILIMELELE DE MAI JOS
\$Prompt = @"
# Change: rework the hand-pose editor — rig-style control skeleton, Blender-like transform controls, universal per-item grips, simulated cases with a timeline, paused world

Rebuild the hand-pose editor (`src/editor/handpose/`) around a **control rig** instead of free-form socket/finger sliders. Read the existing editor, `src/anim/` (weapon_hold, IK, hand-glue, finger data, controller states), `src/player/holdpoint*`, `src/gameplay/` (items, attachments, interactables, vehicles/seats, phone), `src/world/` (time of day, weather, zombies/hordes), `src/ui/` (viewport widget, gizmos) first; present the rig layout and the data model before coding.

## 1. Frozen world while editing
On entering the editor scene: **pause** day/night, weather transitions, wind gusts, VFX time, zombie/NPC simulation, vehicle physics (except what a case needs, see §5), audio ambience; fixed neutral lighting preset (overridable); the editor runs on its own clock. Nothing in the world advances unless the case timeline is playing.

## 2. Control skeleton (what I select and move)
Display a **rig skeleton** for both arms, drawn as bones/joints over the character, with exactly two kinds of selectable controls:
- **IK control bones** (one per hand, plus one elbow pole control per arm): moving a hand IK control moves the **whole arm** through the existing IK solver (shoulder → elbow → wrist), the pole control sets the elbow direction. These are the only way to move arms — no direct shoulder/elbow/wrist FK editing (FK offsets allowed as an advanced toggle later).
- **Finger bones**: thumb (3), index/middle/ring/pinky (3 each) per hand, selectable individually or as a whole finger/whole hand, rotatable per joint with limits; optional per-finger "curl" slider as a shortcut that drives the three joints together.
- A **root item control** at the base of the rig: the item's position, rotation and scale **relative to the hold point / hold frame** (hip, ADS computed from the `sight` socket stays automatic; the root defines the base alignment). This root is the **base of the whole arm alignment**: hand IK controls are children of it, so moving the root carries both hands and arms with it; finger bones are children of the hands. The item mesh itself is **not** directly selectable or draggable — it only moves through the root control. Scale on the root is allowed for calibration (uniform, clamped) and stored per item.
- Hierarchy shown as an outliner: `Root(item) → IK_hand_L/R, Pole_L/R → fingers`. Click a bone in the viewport or the outliner; hover highlights; selected = white, children dimmed-highlight; shortcuts to select parent/child/siblings/mirror.

## 3. Blender-like transform controls
- Viewport gizmo with **translate / rotate / scale** modes (keys `G` / `R` / `S`, mouse-drag with axis constraints `X/Y/Z`, `Shift+axis` for planes, `Ctrl` snapping, numeric typing during a transform, `Esc` cancels, right-click cancels, `Enter`/click confirms), orientation **Global / Local / Parent / Normal**, pivot options (individual origins, median, active), proportional editing off. Transforms respect what the selection allows: fingers = rotate only (plus limits), IK controls = translate + rotate, pole = translate, root = translate/rotate/scale.
- **N-panel-style properties** on the right: Location X/Y/Z, Rotation X/Y/Z (Euler + quaternion readout), Scale, with drag-scrub, typed values, reset-to-default per field, lock toggles, mirror toggle (edit one hand, mirror to the other live), copy/paste transform.
- Undo/redo for every operation; a per-frame "store keyframe" when a case timeline is active (§5).

## 4. Universal grips: the way of holding an item never changes
- A **grip** is defined **once per item** (root + hand IK + poles + fingers) and is used **everywhere** that item is held: hip, ADS (root alignment recomputed from the `sight` socket, hands/fingers unchanged), sprint, crouch, prone, vehicle one-hand, phone, inventory preview, third-person, remote players, spectator. There are **no per-state grip variants** anymore; states only change the hold frame (hold point placement), never the hands on the item. Remove/migrate existing per-state overrides (migration tool: keep the hip version as the grip, report differences).
- Attachments that add a grip (foregrip, angled grip) define their own left-hand sub-grip that overrides the weapon's left hand when mounted.
- Hand-glue guarantee unchanged; the editor shows the glue error and refuses to save a grip whose sockets are unreachable.

## 5. Poses per case: simulated scenarios with a timeline
For interactions whose hands are not simply "holding an item", the editor **teleports me into a simulated case** with the real scene and systems running, and gives a **timeline**:
- **Case definitions** (`assets/handposes/cases/*.json`): spawn the needed props and place the character — e.g. `door_knob`: spawns a door prop with its handle at interaction height, character at the use distance; `door_push`, `lever`, `container_lid`, `pickup_ground`, `ladder_rung` (stub); `vehicle_car_driver`: spawns the car, seats the player, steering wheel visible and selectable as the **case anchor** (its socket position/rotation relative to the seat is editable here — so I can move the wheel/handlebar and the hands follow); `vehicle_moto_driver`, `vehicle_passenger`; `phone_portrait`; `reload_<weapon>` (clip owns the hands — view only, with the hand-off points editable); `mantle_ledge`.
- **Timeline:** scrub the whole motion of the case (approach → reach → contact → use → release) using the real controller/animation/IK pipeline; **pause at any point** (e.g. the contact frame) and edit the hand/finger pose there; poses are **keyframed on the case timeline** (contact pose, hold pose, release pose) and the runtime blends between keyframes exactly as the editor shows; markers for contact/use/release are editable; loop/play/step buttons, speed, onion-skin ghosting of neighbouring keyframes.
- Case anchors (door handle socket, steering wheel socket, lever grip) are the "root" for that case, with the same root control behaviour as items.
- Cases auto-register like items (one stub per interactable type / vehicle seat) and appear in the entry list with the "unreviewed" badge.

## 6. Data
- `assets/handposes/items/<item_id>.json`: single universal grip; `assets/handposes/cases/<case_id>.json`: anchor transform(s) + keyframed poses; style defaults for inheritance; versioned with the migration from the old per-state format. Runtime reads only these; the old finger-curl table is removed.
- Networking unchanged (poses derived locally from item id / case id + state; included in the asset hash).

## 7. Tests
- Rig hierarchy: moving the root moves both hands' IK targets by the same transform; fingers only rotate within limits; item mesh cannot be transformed except through the root; mirror produces the mirrored pose; undo/redo round-trips every operation; universal grip renders identically across all states/views/clients (bit-identical finger/hand transforms for a given item); migration preserves the hip grip; case timeline scrubbing is deterministic and keyframe blending matches runtime; world systems are frozen in the editor (time/weather/zombies unchanged after 600 ticks); hand-glue holds for every item and every case keyframe.

## Deliverables
`src/editor/handpose/` rebuilt (rig, gizmos, N-panel, outliner, cases, timeline, freeze), case JSONs for all interactables/vehicle seats/phone/reloads/mantle, migration tool, runtime updates in `src/anim/weapon_hold*` and interaction/vehicle hand IK, `docs/HANDPOSE.md` rewritten (rig model, shortcuts, universal-grip rule, how to add a case), tests.

Do everything w/o asking me, i will leave my pc and when i come back i want everything done

# Change: phone flow — auto pair on equip, bottom-right screen preview, connect portal with decline fallback, home screen with Camera/Gallery, always-focused, white emissive screen

Rework the phone item behaviour (see `src/gameplay/phone/`, `src/net/http_ws/`, `web/phone/`, hold-point `phone_*` states, `src/render/phone_view*`, `src/ui/`). Read those first; present a plan before coding.

## 1. Equip → pairing starts automatically
- Equipping the phone (hotbar/wheel) immediately starts the local HTTP/WS server (if not running) and enters **pair mode**: no extra key. A pair token and QR are generated at once.
- The in-game phone is raised into the hold-point `phone_portrait` state and **stays there**: remove the "hold right mouse to focus" mode; while the phone is equipped it is always in the focused/camera-ready pose. Lowering it = switching to another item. Mouse look still turns the view; the phone follows the hold point (or the real device's orientation once paired).

## 2. Phone screen preview in the bottom-right corner of the HUD
- While the phone is equipped and **not paired**, render the phone's screen (the same render target the in-game phone shows) as a **HUD panel in the bottom-right corner** (portrait, ~22% of screen height, rounded corners, thin bezel from theme tokens, subtle drop shadow) — this is the "simulated phone screen", mirroring exactly what the phone's screen shows (UI, camera feed, gallery). Mouse/keyboard interact with it: hover the panel to capture the cursor for the phone UI (`Alt` toggle), click apps/buttons; gamepad: hold a button to focus the panel and navigate.
- The panel starts on the **Connect portal** (§3).

## 3. Connect portal → fallback or hand-off
- **Connect portal** (first screen in the HUD panel and on the in-game phone's screen): QR code, the URL in text, a "Waiting for device…" status, and two buttons: `Use in-game phone instead` (decline) and `Cancel`. The portal also appears on the real phone's browser page once the QR is scanned (it asks to allow orientation/camera permissions and shows "Connected").
- **Declined (or no connection within the timeout, or the server can't start):** the HUD panel stays and becomes the in-game phone: it goes to the **home screen** and all interaction happens in the panel + on the held phone's screen; orientation driven by the hold point/mouse.
- **Connected:** the **HUD panel is removed** (slide out + fade); the phone UI is served to the **real phone's page** (home screen, camera, gallery, controls) and the real device's orientation drives the in-game phone; the in-game phone's screen keeps mirroring the same UI so other players see it; if the device disconnects, the HUD panel comes back on the Connect portal with a "Reconnect" button.
- Pair state persists per session (re-equipping a paired phone skips the portal).

## 4. Phone OS: home screen with apps
- **Home screen:** clock (in-game time), date/day counter, signal/battery (cosmetic, battery drains slowly as a later mechanic hook), weather glyph from the weather system, and an app grid with exactly two apps for now: **Camera** and **Gallery** (icons from the icon atlas, data-driven `assets/phone/apps.json` so more apps can be added: map, messages, flashlight later). Swipe/back gestures on the real phone; click/Esc/back in the panel. Status bar + a home button/bar.
- **Camera app:** as implemented (live lens view, zoom slider/pinch, shutter, record, flip front/back stub), opens from the home screen; **Gallery app:** thumbnails grid of photos/videos, open/zoom/swipe, delete, "download on real phone".
- The same UI code renders both to the phone render target (in-game screen + HUD panel) and to the web page (served as the same layout/JSON-driven UI, or a mirrored HTML implementation with the same assets — choose and justify; the web page must match the in-game look).

## 5. Screen look: white with a little bloom
- Replace the phone screen's base material with a **white emissive screen** (the UI is drawn on top of a white background: dark text/icons, light theme) with a **mild bloom contribution** (emissive intensity tuned so it glows subtly, more at night via exposure; cap so it never blows out in daylight; `phone_screen_emissive` cvar). Screen glass with slight reflection/fresnel over it; the white screen also acts as a small light source on the hands/face at night (point light at the screen socket, low intensity).
- Off state (phone not equipped / screen locked): screen black.

## 6. Data, settings, networking
- `assets/phone/apps.json`, `assets/phone/theme.json` (light theme tokens for the phone OS), panel size/position in HUD settings, pair timeout, auto-pair on/off (default on).
- Networking unchanged: pairing is local; the in-game phone's screen content is rendered locally on every client from the phone's replicated state (mode/app/zoom), no streaming between players.

## 7. Tests
- Equip → server up + pair mode within one tick; portal shown in the panel; decline → home screen in panel, no device required; simulated device connect (headless WS client) → panel removed, orientation stream applied, disconnect → panel returns with Reconnect; home → Camera → photo → Gallery shows it, both on the in-game render target and via the web page; screen emissive is white with bloom within the configured range in day and night renders; no "hold right click" path remains (input map has no phone focus bind); panel layout at 16:9/21:9/4:3 uses theme tokens only.

## Deliverables
Updated `src/gameplay/phone/` (flow, OS/home screen, apps), HUD phone panel widget, Connect portal (in-game + web), `web/phone/` home/camera/gallery, white emissive screen material + light, `assets/phone/*`, settings, `docs/PHONE.md` updated with the new flow, tests.

Do everything w/o asking me, i will leave my pc and when i come back i want everything done

"@

# 2. Setează timpul de așteptare (2 ore = 7200 secunde)
\$WaitSeconds = 7200

Clear-Host
Write-Host "=== INSTALARE PROMPT AUTOMAT CLAUDE ===" -ForegroundColor Cyan
Write-Host "Se incepe numaratoarea inversa pentru \$WaitSeconds secunde (2 ore)..." -ForegroundColor Yellow

TotalTime = WaitSeconds
while (\$WaitSeconds -gt 0) {
    \(TimeSpan = [TimeSpan]::FromSeconds(\)WaitSeconds)
    TimeFormated = "0:D2:1:D2:2:D2" -f TimeSpan.Hours, TimeSpan.Minutes, TimeSpan.Seconds
    
    Write-Progress -Activity "Asteptare resetare limita Claude" -Status "Timp ramas: TimeFormated" -PercentComplete ((TotalTime - WaitSeconds) / TotalTime * 100)
    Start-Sleep -Seconds 1
    \$WaitSeconds--
}

Write-Host "`n[!] Limita a expirat! Se aduce VS Code in prim-plan..." -ForegroundColor Green

# Interfața Windows API pentru focus ferestră
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class User32 {
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
}
"@

$vsCodeProcess = Get-Process -Name "Code" -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1

if ($vsCodeProcess) {
    [User32]::SetForegroundWindow($vsCodeProcess.MainWindowHandle)
    Start-Sleep -Milliseconds 800
    $wshell = New-Object -ComObject WScript.Shell
    
    # --- PASTE PROMPT ---
    Write-Host "[>] Se trimite promptul in chat..." -ForegroundColor Yellow
    Set-Clipboard -Value $Prompt
    $wshell.SendKeys("^v")       
    Start-Sleep -Milliseconds 600
    $wshell.SendKeys("{ENTER}")  
    
    Write-Host "`n[✓] Promptul a fost lipit si trimis automat!" -ForegroundColor Green
} else {
    Write-Host "[X] Eroare: Nu am gasit nicio instanta deschisa de VS Code (Code.exe)!" -ForegroundColor Red
}
