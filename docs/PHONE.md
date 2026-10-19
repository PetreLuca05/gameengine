# In-game phone and the companion page

Every player carries a phone (it is built in, outside the inventory). **X** takes it out over
whatever is selected (a weapon slot, the hands) and puts it away again; choosing a weapon also puts
it away.
While it is out it is **always raised** in the focused, camera-ready pose (hold point state
`phone_portrait`): there is no aim / "hold right mouse" mode. Mouse look still turns the view.

The phone's **screen is not on the phone model**. It is shown in two places only:
- the **HUD phone panel** (the simulated phone screen, bottom right), for the local player;
- the **companion web page** on your real phone, once it is paired (in the menus: see "Phone
  flow"). Then there is no panel and the real phone's orientation drives the in-game phone.

The held model's screen is plain dark glass (no texture, no glow, no light), for everyone. Other
players and spectators only see the pose (raised, portrait / landscape, the real phone's rotation).

Everything stays on the local network: the game is the server, and nothing goes through the cloud.

## Phone flow (pairing lives in the menus)

Pairing is a **session decision made in the main menu**, not something that happens when the phone
item comes out. The HTTPS / WebSocket server starts at menu start (`phone_auto_pair 1`, the default)
or when the Connect portal opens, and stays up for the whole run: maps, lobbies and "Leave to main
menu" never stop it.

```
MAIN MENU  Player tab: Phone card  /  footer: phone icon + status
   │  click (card, footer icon, pause card, "Connect your phone" on the in-game home screen)
   v
CONNECT PORTAL (centred modal: QR, URL, "Waiting for device…", Use in-game phone, Close)
   ├─ device connects ─────> mode DEVICE  (card: Connected (iPhone) · Disconnect · Forget device)
   ├─ Use in-game phone ───> mode INGAME  (card: In-game phone · "Connect a device")
   └─ Close ───────────────> nothing decided (card: Not connected)

MATCH   equip (X / a weapon key) never opens the portal:
   device connected ──> the phone just comes up; the real phone drives it (no HUD panel)
   no device ─────────> the in-game phone (HUD panel), as before
   device drops ──────> toast "Phone disconnected — using in-game phone", the panel slides back
   page reconnects ───> real-phone mode again, silently (also from the pause menu's Phone card)
```

- **Card states** (`phone_os_card`): `Not connected`, `Waiting…` (the portal is open),
  `Connected (<device>)`, `In-game phone`. The same card is on the main menu's Player tab and in the
  pause menu (top right, hidden while a sub-menu is open); the footer of the main menu shows a small
  phone glyph with the same status next to the Steam status (click: the portal).
- **Session `phone_mode`** (`phone_mode` prints / sets it): `unset` until the portal is answered,
  `ingame` after "Use in-game phone" or `Disconnect`, `device` while a device is (or was last)
  connected. It is per run and per client: nothing about it goes over the network.
- **Disconnect** closes the page's socket (close code 4002: the page shows "Disconnected" with a
  Reconnect button, it does not retry by itself) and switches the session to the in-game phone; the
  stored device stays, so the next launch reconnects it. **Forget device** revokes the session
  (4001: the page forgets it) and deletes the stored device.
- **Persisted pairing.** A paired device's session key is stored in `<pref dir>/phone/device.json`
  (key, device name, expiry; `phone_device_days`, 30 days, sliding on every connect, 0 = never
  stored). The server restores it when it starts, and the page keeps the key in `localStorage`: a
  page left open on the phone reconnects on its own as soon as the game is back (it probes
  `/ping?s=<key>`; a 403 means the session ended and it asks for a new QR scan).
- **The companion page in the menu** shows the **menu companion screen**: a "Connected to <PC
  name>" banner, Home with Camera / Gallery greyed out ("Available in a match"). In a match the
  apps unlock and the page behaves as before.
- **Lobby:** members with a connected device get a tiny phone icon in the lobby's player list (one
  flag bit in the member flags of the lobby snapshot, set by the `LI_PHONE` intent; cosmetic).
- `phone_auto_pair 0`: the server starts only when the portal opens (or `phone_server 1`).

The flow is pure (`phone_os.c`: `PhoneOs`, `phone_os_portal_open / portal_close / decline /
connected / disconnected / disconnect / equip / card`), unit tested.

## The phone OS

| App | In the HUD panel / on the page |
|---|---|
| **Connect portal** | not on the phone any more: a centred menu modal (`SCR_PHONE`, `src/ui/mainmenu/mm_phone.c`, `assets/ui/screens/mainmenu_phone_portal.json`): QR, URL, code expiry, status, `Use in-game phone`, `Close` (`Disconnect` with a device). The page's own portal shows "Connected" and asks for motion access ("Allow motion & continue"), "Disconnected" + Reconnect after the game's Disconnect, "Pairing ended" when the session is gone. |
| **Home** | status bar (in-game clock, LINK when paired, cosmetic signal, battery), the big clock, day + part of day, the weather glyph + name from the weather system, the app grid, the home bar; "Connect your phone" (in-game phone only) opens the Connect portal. On the page: a "Connected to <PC name>" pill; in the menus (status `ctx` "menu") Camera / Gallery are greyed out ("available in a match"). |
| **Camera** | the live lens view, zoom chips 1x / 2x / 5x + log slider (wheel / pinch), shutter, record, last-shot thumbnail, front / back switch. |
| **Gallery** | thumbnails grid (photos + videos, newest first, wheel scroll), viewer (prev / next or swipe, wheel / pinch zoom), Delete; Download / Share on the page. |

- **Apps are data:** `assets/phone/apps.json` lists the home grid (id, label, icon tile, the
  built-in app it opens). An entry without a built-in app shows "coming soon" (map, messages,
  flashlight later). Weather presets map to glyphs there too.
- **Look:** `assets/phone/theme.json` (light theme: white background, dark text, accent blue)
  plus the HUD panel tokens (readout gap, radius, bezel, shadow, slide time).
- **Icons:** one atlas, `assets/phone/icons.png` (128 px tiles: camera, gallery, weather glyphs),
  generated by `tools/phone_icons.py` (Python stdlib, signed distance shapes).
- **Battery** drains slowly while the phone is out (`phone_battery_drain`, cosmetic, never below
  5 %): a hook for a later mechanic.

### Why the page is a mirrored HTML implementation

The in-game screen is a CPU canvas (`phone_ui.c`) because it must be an image (the HUD panel, the
lens composite). The page is HTML: native text at the phone's DPI, touch gestures, scrolling,
`<video>`, and the browser's Download / Share sheets need a real DOM; streaming the in-game UI as
pixels would be blurry and laggy. To make both **look the same** they share the assets: the game
injects `theme.json`, `apps.json` and the icon atlas (as a data URI) into the page at the
`/*DG_PHONE_DATA*/` marker (`phone_page_build`). The current app is synced both ways: the page
sends `{"c":"app","v":"camera"}`, the game's status carries `"app"`.

### Input

| Input (default) | |
|---|---|
| **X** / pad d-pad down | take out / put away |
| **Left Alt** (`phoneui`) | free the cursor for the HUD panel (again: back to mouse look). Hover + click its buttons; **Esc** = back (viewer → grid → home), then releases; a click outside the panel releases; the wheel scrolls / zooms the panel. Alt is shared with walk on purpose. |
| **Right stick click** held (`phonenav`) | gamepad: the d-pad / stick moves the focus over the panel's buttons, A presses, B goes back (the game controls pause meanwhile) |
| **Left mouse** / **R** / **T** / **wheel** | Camera app with the mouse in the game: photo / record / selfie / zoom |
| **J** | Gallery ↔ home |

On the page: tap, the home bar (or a swipe up from the bottom edge) for home, a swipe right from the
left edge for back, swipes in the viewer for prev / next, pinch to zoom.

## The HUD panel

- Height `hud_phone_size` × the screen height (0.22), portrait at the phone screen's aspect, in the
  `hud_phone_corner` corner of the HUD safe area (0 bottom right). Bottom right it sits
  `readout_gap` above the item / ammo readout. Bezel, radius and shadow come from
  `assets/phone/theme.json`, the opacity from `hud_opacity`. Settings → HUD → Phone panel.
- The image is the phone's screen (UI over the lens view in the camera app), **box-filtered down to
  the panel's pixel size** with rounded corners as alpha (`shaders/post/phone_panel.comp`), so text
  stays clean at 22 %.
- The panel rect is a pure function (`phone_panel_rect`), unit tested at 16:9, 21:9, 4:3 and more.

| Area | Code | Data |
|---|---|---|
| Phone module: server lifecycle (`phone_tick`, every frame from the main loop), page messages, pairing API for the menus (`phone_pair_info`, `phone_portal_show / hide`, `phone_decline / disconnect / forget`), the stored device, orientation, capture, commands | `src/gameplay/phone/phone.c` | |
| Phone card, Connect portal modal, footer chip, phone glyph | `src/ui/mainmenu/mm_phone.c`, `SCR_PHONE` in `src/ui/menus.c` (pause menu card) | `assets/ui/screens/mainmenu_phone_{card,portal}.json`, `mainmenu_player.json` (`phone`), `pause.json` (`phone`), theme tokens `ph_*` |
| Lobby phone flag (`LI_PHONE`, member flag bit 3) | `src/net/lobby.c`, `lobby_net.c`, `src/ui/mainmenu/mm_lobby.c` | |
| Phone OS (pure): flow, session mode, card state, device names, the stored device, apps, layout + hit tests, assets, HUD panel rect, page injection | `src/gameplay/phone/phone_os.c` | `assets/phone/apps.json`, `theme.json`, `icons.png` (`tools/phone_icons.py`) |
| Screen UI (CPU canvas), HUD panel + hints, the phone model (dark glass) | `src/gameplay/phone/phone_ui.c` | |
| Orientation math (pure) | `src/gameplay/phone/phone_orient.c` | |
| Stream rate control + JPEG (pure), the JPEG-over-WebSocket sink | `src/gameplay/phone/phone_stream.c`, `phone_stream_ws.c` | |
| Photos, sidecars, gallery (pure) / H.264 MP4 recording / QR codes | `phone_capture.c`, `phone_video.c`, `phone_qr.c` | |
| Wire protocol (shared with the page) | `src/gameplay/phone/phone_proto.h` | |
| HTTPS + WebSocket server, tokens, TLS + certificates | `src/net/http_ws/` | |
| Lens camera, UI-only views, screen + HUD panel images, light post, readback | `src/render/phone_view.c`, `shaders/post/phone_tonemap.comp`, `phone_panel.comp` | |
| Always raised while equipped (no aim button) | `src/gameplay/weapons.c` (camera items) | |
| Hold point states, driven rotation | `src/player/holdpoint.c`, `src/anim/weapon_hold.c` | `assets/human/holdpoint.json` (`phone`, `phone_landscape`) |
| Hold styles | `src/anim/procedural.c` | `assets/human/controller.json` |
| Item | `src/gameplay/items.c` (`"camera"`, `"builtin"`) | `data/items.json` (`phone`) |
| Companion page | `web/phone/index.html` | |
| Settings | `src/ui/screens/scr_settings.c` (Phone; HUD → Phone panel) | |
| Tests | `tests/test_phone.c`, `tests/test_anim.c` (`test_phone_hold`), `tools/tests/phone_{flow,ws,screen}.py`, `phone_e2e.sh`, `phone_net.sh` | |

## Pairing details

1. In the main menu, open **Player › Phone › Connect** (or click the phone in the footer; in a match:
   the pause menu's Phone card, or "Connect your phone" on the in-game phone's home screen). The
   Connect portal opens with a QR code.
2. Its QR code encodes `https://<lan-ip>:47820/?t=<token>`; the address is also shown as text, in
   case your camera can't read the code.
3. Scan the code with the real phone. It must be on the same Wi-Fi.
4. The first time, the browser warns about the certificate (see the next section). Accept it.
5. The page's portal says **Connected**: tap **Allow motion & continue**. On iOS, allow **Motion &
   Orientation** access when asked. The page opens the home screen; the game's portal shows
   "Phone connected" and closes, the card shows **Connected (iPhone · Safari)**.
6. In a match, take the phone out (X) and hold the real phone upright, facing the direction your
   character is looking. The first sample calibrates automatically. Later, use **Calibrate** in the
   page's menu, or double-tap the picture.

Pairing rules:
- The token is good for **one use** and **expires after 2 minutes** (`phone_token_ttl`); an expired
  code on an open portal is replaced by a new one automatically.
- Using it creates a session key, which the page keeps in `localStorage` and the game in
  `<pref dir>/phone/device.json` (`phone_device_days`, sliding). Reloads (iOS forgets the motion
  permission on every reload), media downloads and the next launch use the session.
- Each player has one phone. Pairing another phone, or opening the page in a second tab, replaces the
  first one, which sees "Paired elsewhere".
- **Disconnect** (card) closes the page's socket for this session; **Forget device** (card, the
  page's "Forget this phone", `phone_forget` / `phone_unpair`) revokes the session.
- Pairing is per client and local: nothing about it goes over the network except the cosmetic
  lobby flag.

## HTTPS and permissions (read this before blaming the code)

Browsers only give `DeviceOrientationEvent` to **secure contexts**. **iOS Safari** (13 and later)
also needs `DeviceOrientationEvent.requestPermission()`, called from a tap. Over plain
`http://192.168.x.x` it never fires. The page has no other way to get the phone's orientation, so
the server uses **HTTPS by default** (`phone_tls 1`).

The page never uses the phone's own camera or microphone, because the feed goes game → phone. Only
motion access needs HTTPS.

How the certificate works:
- On first start the game creates a **local root certificate**: EC P-256, valid for 10 years,
  stored in `<pref dir>/phone/certs/ca.crt` + `ca.key`.
- From it, the game issues a **server certificate for the current LAN IP**: IP subjectAltName,
  serverAuth, valid for 397 days.
- The server certificate is re-issued automatically when the IP changes or when it gets within
  30 days of expiry. Because the root stays the same, any trust you gave it keeps working.

Two ways to use it:
- **Quick (every new phone).** Accept the warning once.
  - iOS Safari: *Show Details → visit this website → Visit Website*.
  - Android Chrome: *Advanced → Proceed to …*.

  The page then counts as a secure context: the connection is encrypted with the game's own
  certificate, and motion sensors work.
- **Clean (no warnings).** Install the root certificate on the phone. It's served over plain HTTP
  on the next port, `http://<lan-ip>:47821/ca.crt` (also at `https://…/ca.crt`).
  - iOS: open it, then *Settings → General → VPN & Device Management → install the profile*, then
    *Settings → General → About → Certificate Trust Settings → enable full trust*.
  - Android: *Settings → Security → Encryption & credentials → Install a certificate → CA
    certificate*. Chrome trusts user-installed CAs.

The game shows the root certificate's SHA-256 fingerprint (`phone_stream_stats`, and the log at
start) so you can compare it with what the phone reports.

Known limitations:
- **Not verified on real devices in this change.** The browser behaviour described here is from
  the platform documentation, and the automated test uses desktop Chromium with synthetic
  orientation events. Check on a current iPhone and Android phone before relying on it.
- **iOS asks for motion permission again on every page load.** The page reconnects with its session
  key, so it's one tap.
- **iPhone Safari has no Fullscreen API for pages.** The page uses the full viewport (`100dvh`,
  safe areas). *Add to Home Screen* gives a standalone, chrome-free window.
- **The screen must stay on while streaming.** The page asks for a Wake Lock, which recent iOS and
  Android support.
- **`phone_tls 0` (plain HTTP)** works for the feed, photos and video. Motion access then depends on
  the browser and is generally unavailable. Android Chrome can be told to treat the origin as secure
  via `chrome://flags/#unsafely-treat-insecure-origin-as-secure`.
- **Some networks isolate clients** (guest Wi-Fi, "AP isolation"), and then the phone can't reach
  the game at all. Corporate or school networks may block the ports too.
- **Windows Firewall** asks the first time the game listens. Allow private networks.

## Security

- The server binds to the **LAN interface** in the QR code (`phone_bind auto`; or `any`, or an
  explicit IPv4 address).
- Connections from **outside the private ranges are refused** (10/8, 172.16/12, 192.168/16,
  169.254/16, loopback).
- The WebSocket needs a **token** (one use, 128 bits, 2 minutes) or a **session key** (128 bits). Both
  come from mbedTLS's CTR-DRBG seeded by the operating system, and both are compared in constant
  time.
- **Rate limits per IP**: connections in a token bucket (burst 20, then 10 per second); 5 failed
  token or session checks within 60 s ban the address for 60 s.
- **No file system access.** The only things served are the page (held in memory), `/ca.crt`, and
  photos and videos the game registered for that player's session (by numeric id). Requests without
  the session get 403.
- The page is sent with `Referrer-Policy: no-referrer`, and the token leaves the address bar as soon
  as the session exists. A strict CSP allows no third-party loads.
- Limits: 16 connections at most, 64 KB maximum WebSocket message, 8 KB maximum request headers,
  10 s idle timeout before the WebSocket upgrade, 20 s silence timeout after it (the server pings
  every 5 s).

## Streaming architecture

```
render target (lens view, 640x1136 @ 30 Hz) --copy--> host-cached buffer (frame slot N)
   ... two frames later (fence passed) ... --> phone.c --memcpy--> sink slot (3 slots)
   --> job: JPEG (stb_image_write, ~10 ms) --> hws_send(droppable) --> server thread --> TLS --> page
page: newest frame wins -> createImageBitmap -> canvas -> {"c":"ack","f":id}
```

Choices:
- **JPEG frames over the paired WebSocket.** This is option (c) in the original brief: the
  baseline, in pure C. It needs nothing beyond TLS, it works on every browser, and the same socket
  carries orientation, controls and acks. MJPEG in an `<img>` tag was rejected: it can't measure
  latency, it can't drop frames sensibly, and iOS handles it poorly.
- **No stall anywhere.** The GPU copy lands in a ring of host-cached buffers, read once that frame
  slot's fence has passed (two frames later). Encoding runs on the job system. A frame waiting in the
  socket queue is replaced by a newer one (droppable), never queued behind it.
- **Adaptive rate** (`PhoneRate`, unit tested). It counts congestion: the transport backlog over
  budget, more than 4 frames without an ack, or acks slower than 250 ms. Under congestion it first
  lowers JPEG quality (`phone_quality` down to 35), then the frame rate (down to 10 Hz), then the
  resolution (×0.75, then ×0.5). Every 2 clean seconds it recovers one step, in reverse order.
- **Latency.** The page acks every displayed frame and pings once a second. The game estimates
  glass-to-glass as (ack time − render time − half the page's ping), shown on both sides and in
  `phone_stream_stats`.
  - **Budget:** about 33 ms average frame wait at 30 Hz, about 10 ms encode, LAN transfer of a
    50–120 KB frame, about 10 ms decode, one display frame. That adds up to well under the
    150 ms target on a normal LAN.
  - **Not measured on real hardware here:** the test machine renders at about 1 fps in software,
    which dominates its numbers.
- **Other players' phones** show no screen (the model is dark glass): only the pose is replicated
  (`ANS_PHONE`: flags + rotation). Nothing about the screen content goes over the network.

### Upgrading to WebRTC

`PhoneStreamSink` (`phone_stream.h`) is the seam: `push(rgba, w, h, info)`, `on_ack`, `update`,
`stats`, `destroy`. A WebRTC sink would:
1. use libdatachannel's C API (MPL-2.0) for ICE, DTLS and SRTP, signalling over the existing
   paired WebSocket (offer / answer / candidates as JSON messages);
2. encode H.264 (the minih264 encoder already in the tree, or a hardware encoder) or VP8, and
   send it as an RTP video track;
3. carry orientation on an unreliable, unordered data channel.

The page would swap its canvas for a `<video>` element fed by `RTCPeerConnection`. The game side,
including capture, rate stats and acks, keeps its shape.

Audio (game mix → Opus) is not implemented: the stream and the videos are silent for now. The
recorder has no audio track; MP4 + Opus or AAC via minimp4 is the planned path.

## Orientation: real phone → in-game phone

The page reads `deviceorientation` and converts it to a quaternion,
`q = Rz(alpha) Rx(beta) Ry(gamma)` (device → earth), mirrored in C as `phone_device_quat`. It
sends 16-byte binary samples at 60 Hz. The game then:

1. Converts axes: `phone_orient_game` maps earth (east, north, up) to game axes (x, y up, -z). The
   item frame equals the device frame: +z is out of the screen, and the lens looks down -z.
2. Calibrates: `phone_heading` + `phone_apply_calibration`. **Only the heading is removed**:
   gravity keeps pitch and roll absolute, so a sloppy calibration doesn't tilt the world. The first
   sample calibrates automatically; Calibrate and double-tap re-centre.
3. Applies sensitivity and per-axis inversion (`phone_shape`).
4. Smooths with a **1-euro filter**. `phone_smoothing` sets the minimum cutoff (0.6–4 Hz) and the
   speed coefficient is 0.5, so slow motion is steady and fast turns stay within about 2 frames.
5. Drives the hold point. The rotation is relative to the view yaw: mouse look still turns the
   body, and the phone adds to it. It is converted to **camera space** (view pitch removed),
   **clamped to the reach cone**, and applied by the hold point as a **rotation about the screen**:
   - reach cone (`phone_clamp_reach`): ±55° yaw, ±70° pitch, ±100° roll, compressed smoothly
     past 80 % of each limit, never exceeded;
   - the hold point uses `rot_cam` / `rot_w` (`holdpoint.c`);
   - the hand IK follows by construction: the item is finalised from the solved right hand, and
     the left hand is solved onto the result.
6. Switches to landscape when the roll goes past 55° (back below 35°). The hold style becomes
   `phone_landscape` (two-handed), and the state `phone_landscape`.
7. Falls back on disconnect: after 0.5 s without samples the rotation weight eases back to the
   mouse aim over 0.3 s (`ProcState.phone_w`).

Replication:
- Clients send the packed rotation in `UserCmd.phone_rot`: smallest-three, 32 bits, about 0.1°
  error.
- Flags go in `phone_flags`: landscape, driven, recording, front camera.
- The host copies them into the character state (`ANS_PHONE`, 5 bytes), only while the phone is out (it is always raised then).
  Remote copies interpolate the rotation between snapshots and solve the same pose.
- This bumped `DG_PROTOCOL_VERSION` to 12.

`phone_fake_orientation <pitch> <yaw> <roll>` drives the phone without a device, for tests. `off`
returns to the mouse.

## The in-game phone

- **Model.**
  - The phone is a box body, with a screen quad on +z and a lens bump on the back.
  - Item space: origin = the right-hand grip (`grip_r`), +y = top of the screen.
  - Socket defaults come from `"camera"` in `items.json`: `screen` (also the hold point "sight"),
    `lens` (looks -z), `front_lens` (above the screen, looks +z), `anchor` (= grip_r).
  - `grip_l` is used by the landscape style.
- **Hold.**
  - `hip` / `sprint`: lowered at the chest, screen tilted towards the eyes.
  - `phone_portrait` / `phone_landscape`: their `pos` / `rot` place the **screen socket** in
    camera space (right, up, forward), not the anchor. Tune them with the holdpoint editor (F8).
- **Screen.** The model's screen quad is **dark glass** (a near-black dielectric: real reflections,
  no texture, no emissive, no light), for the owner, other players and spectators alike. The
  phone's UI lives on the HUD panel and the paired page only (`phone_status` reports
  `model_screen=<with content>/<drawn>`, always `0/n`).
- **Lens view** (the camera app, photos, video, the stream):
  - the lens camera has its own FOV: 68° vertical at 1x; zoom narrows it as
    `2·atan(tan(fov/2)/zoom)`;
  - rendered at `phone_res_w × phone_res_h` (640×1136) at `phone_fps` (30 Hz), only in the camera
    app; the other apps are **UI-only views** (no scene pass: the overlay over black);
  - opaque scene + characters + particles + rain, sky, then the **light post**
    (`phone_tonemap.comp`): the main view's auto exposure + `phone_exposure`, the user's tonemapper
    (ACES / PBR Neutral), vignette — no DoF, motion blur or bloom.

  It writes:
  - the clean sRGB camera image (stream, photos, video);
  - the screen image, with the **phone OS** composited on top (drawn on the CPU, white background
    outside the camera app, uploaded when it changes);
  - from that, the **HUD panel image** at the panel's pixel size (`phone_panel.comp`).
- **Front (selfie) camera.** The owner's body is drawn shadow-only in first person; `MAT_SELF`
  makes it visible to phone cameras (skinned pass 3), head included. The in-game preview is mirrored;
  photos, videos and the stream are not.

## Photos and videos

**Photos.**
- With `phone_photo_hires 1` the frame is re-rendered at 1080×1920; otherwise the stream
  resolution is used.
- Then: unsharp mask, vignette, filter (`phone_filter`: none / vivid / warm / cool / mono / noir),
  and JPEG at quality 92 on a job. Landscape photos are rotated upright.
- Saved as `<pref dir>/screenshots/phone/YYYY-MM-DD_HH-MM-SS[_n].jpg`, plus a JSON sidecar.
- Sidecar fields: kind, file, width, height, zoom, filter, map, position, yaw, pitch,
  `game_time { day, hours, text }`, weather preset, created.
- Feedback: a shutter flash on the screen, the thumbnail on the phone, a `media` message to the
  page, and the JPEG pushed as a blob for **Share / save** (the iOS share sheet saves to Photos).

**Videos.**
- **H.264 in MP4**: the minih264 encoder (CC0) with the minimp4 muxer (CC0), at the stream
  resolution rounded down to multiples of 16, `phone_video_kbps` (4000), one key frame per second.
- Encoded in order on the job system from a 6-frame FIFO; frames are dropped when the FIFO is full.
- Timestamps come from the capture clock, and the last frame lasts until Stop, so dropped frames
  don't change the clip length.
- Saved as `<pref dir>/videos/phone/<stamp>.mp4`, plus `.json` and `.thumb.jpg`.
- `phone_record_max` (300 s) stops recording automatically.
- **No audio track yet.**
- Patent note: shipping an H.264 encoder can carry MPEG LA licensing obligations for a commercial
  product. VP8 / VP9 in WebM via libvpx (BSD) is the royalty-free alternative, with weaker iOS
  playback.

**Gallery.**
- Both folders are scanned when the phone first comes out, newest first.
- In game: J (thumbnails decoded lazily, one per redraw).
- On the page: the thumbnail button, then a viewer with Download (`?dl=1`) and Share / save.

## Console and cvars

| Command | |
|---|---|
| `phone_out [0|1]` | take the phone out / put it away (X) |
| `phone_server on|off` | start / stop the server (`phone_server` cvar: 0 off, 1 on, 2 automatic — with the main menu when `phone_auto_pair` is on, else the first Connect portal) |
| `phone_pair`, `phone_portal [open|close|decline]` | the Connect portal (modal), URL printed to the console |
| `phone_decline`, `phone_mode [ingame]` | "Use in-game phone" for the session; `phone_mode` prints unset / ingame / device |
| `phone_disconnect` | the card's Disconnect (the in-game phone this session; the stored device stays) |
| `phone_forget` (= `phone_unpair`) | Forget device: revoke the session, delete the stored device |
| `phone_card` | the Phone card as one log line (tests) |
| `phone_fake_orientation <pitch> <yaw> <roll>` / `off` | drive the phone without a device |
| `phone_stream_stats` | resolution, quality, fps, kbps, encode ms, RTT, glass-to-glass, server counters, orientation source |
| `phone_photo`, `phone_record`, `phone_zoom <x>`, `phone_selfie [0|1]`, `phone_landscape [0|1]`, `phone_screen home|camera|gallery|pair`, `phone_calibrate` | the same as the keys / page buttons |
| `phone_back`, `phone_ui [0|1]` | back, free the cursor for the panel |
| `phone_button <name> [index]`, `phone_tap <x> <y>` | press a screen button / tap at design px (the same path as a click on the panel) |
| `phone_status`, `phone_wait <flow|app|card> [frames]` | one status line (flow, app, panel, mode, portal, the top screen ...); hold a script until the phone gets there (tests) |

Cvars:
- `phone_port`, `phone_tls`, `phone_bind`, `phone_pairing`, `phone_token_ttl`, `phone_auto_pair` (the server
  starts with the main menu, so a remembered phone reconnects by itself; the request's
  "phone_autopair"), `phone_device_days` (30; 0 = nothing stored)
- `phone_res_w`, `phone_res_h`, `phone_fps`, `phone_quality`
- `phone_smoothing`, `phone_sensitivity`, `phone_invert_yaw`, `phone_invert_pitch`,
  `phone_invert_roll`
- `phone_filter`, `phone_photo_hires`, `phone_exposure`, `phone_battery_drain`
- `phone_video_kbps`, `phone_record_max`
- `hud_phone_size`, `hud_phone_corner` (Settings → HUD)

Most of them are also in **Settings → Phone**.

## Libraries

| Library | Licence | Use |
|---|---|---|
| mbedTLS 3.6.4 LTS (FetchContent) | Apache-2.0 | TLS 1.2 / 1.3 server, certificate generation, CSPRNG |
| Project Nayuki qrcodegen (C) | MIT | QR codes |
| quirc | ISC | QR decoding in the tests only |
| minih264 | CC0 | H.264 encoder |
| minimp4 | CC0 | MP4 muxer |
| stb_image_write (already vendored) | public domain / MIT | JPEG |

The HTTP/1.1 + WebSocket server is our own, in about 1,000 lines. mongoose was rejected because it
is GPLv2 or commercial; civetweb because its TLS needs OpenSSL DLLs.

## Tests

- **`ctest -R phone`** (`tests/test_phone.c`, 890 checks):
  - QR encode → decode with quirc;
  - token auth: wrong, empty, prefix, expired at exactly 2 minutes, replayed, re-pair, unpair;
    session lifetime (sliding on resume, expiry), stored sessions restored (malformed keys refused);
    rate limit + bans;
  - HTTP parsing, the RFC 6455 accept-key example, fragmented frames + an interleaved ping fed byte
    by byte, unmasked / oversized frames rejected;
  - orientation reference poses (flat, upright, turned, gamma, tilted), calibration at any heading,
    heading continuity from flat to upright, Euler round trip, sensitivity / inversion;
  - 1-euro noise and lag, reach clamp (5000 random poses stay inside, continuity), quaternion
    packing error;
  - the JPEG pipeline at 30 fps on the job system (render-thread cost under 5 ms, 0 skipped);
  - photo JPEG + sidecar parsed back; a 2 s MP4 with its boxes, duration and size parsed back;
  - a headless WebSocket client against the real server: page, no file system, pair, orientation
    sample, 100 streamed frames, photo download, 2 s video download, resume with the session,
    unpair; `/ping` (200 / 403), the game's Disconnect (close 4002, the session stays, the page
    resumes), a second server (the next launch) refusing the key until the stored device is
    restored, then resuming it;
  - TLS: certificate generation, a handshake that verifies the IP SAN against the CA, the CA over
    the HTTP helper, CA reuse, a new server certificate for a new IP;
  - phone OS: the flow (equip never pairs: the in-game phone; the portal opens / closes / declines,
    no server, connect in the menu, equip with a device = no panel, a mid-match drop → panel +
    toast with the mode kept, silent reconnect, Disconnect, Forget), the card states, device names
    from user agents (iPhone, iPad, Pixel, the model hint, Samsung Internet), the stored device
    JSON (round trip, expiry, malformed), every app's buttons on
    screen and hit-tested at their centres, the gallery paging, the zoom slider round trip,
    `apps.json` (exactly Camera + Gallery, atlas tiles, a glyph per weather preset, a data-only
    extra app), the atlas size, `theme.json`, the page injection (`</` escaped, missing assets),
    the HUD panel at 1920×1080 / 2560×1080 / 1440×1080 / 3440×1440 / 1280×720 / 1024×768 in all four
    corners (22 % high, phone aspect, inside the safe area, clear of the item readout), gallery
    delete (file + sidecar).
- **`ctest -R anim`** (`test_phone_hold`): the phone driven by random poses (some far outside the
  cone) in portrait and landscape.
  - Right hand on the phone: 0.04 mm portrait, about 6 mm at the landscape extremes (the arm is
    never stretched).
  - Left hand glued: 0.04 mm.
  - Item drift: 1e-7.
  - The camera-space rotation never leaves the reach cone.
- **`ctest -R lobby`**: `LI_PHONE` (accepted in any state, no chat line, ignored twice), the phone
  flag in the snapshot; the theme-token lint covers `mainmenu_phone_card.json` and
  `mainmenu_phone_portal.json` (tokens only, inside the root at six resolutions, no overlaps).
- **`tools/tests/phone_flow.py`** (from `build/bin`, runs `phone_flow.cfg`, no device): equip → one
  frame later the in-game phone (home, panel), no portal, no code; raised without any aim input;
  "Connect your phone" → the portal modal (server + code); decline → `phone_mode ingame`, the
  portal closes; Camera → photo → Gallery → viewer → back; re-equip: no portal; equipping never
  showed the portal; the HUD panel measured in screenshots at 1600×900 / 2100×900 / 1200×900;
  static: no phone focus bind, the phone never reads aim. Screenshots `phone_flow_*.png`.
- **`tools/tests/phone_ws.py`** (from `build/bin`, `--menu`, runs `phone_ws.cfg` then
  `phone_ws_relaunch.cfg`): a simulated device in Python's stdlib (TLS + WebSocket).
  - The main menu: the server is up (auto-pair), card Not connected → the Player tab's portal
    (modal, code; card Waiting…) → pairing; the page's status says `ctx` menu, card Connected
    (iPhone · Safari), the footer shows it.
  - A LAN lobby lists the phone flag; in the match the status says `ctx` match, equipping goes
    straight to the device (no portal, no panel), orientation drives it, Camera (frames), photo
    (blob), Gallery (listed, downloads with the session).
  - The socket drops: the in-game phone + the toast; `/ping` still 200; the page resumes: paired
    again. The pause menu's card opens the portal; closing it keeps the device.
  - Relaunch: the stored device is restored, the page reconnects with its key (no code), Forget
    device closes it with 4001 and deletes the file.
  - Screenshots `phone_ws_*.png` (card, portal, card connected, lobby, match, camera, dropped,
    pause, pause portal).
- **`tools/tests/phone_screen.py`**: the model's screen is dark glass day / night and in the camera
  app (`model_screen=0/n`), no glow, while the panel shows the white UI.
- **`tools/tests/phone_e2e.sh`**: the game plus headless Chromium driving the real page.
  - Needs an existing Playwright module (`PLAYWRIGHT_PATH`) and Chromium (`CHROMIUM`); nothing is
    installed.
  - Covers: HTTPS secure context, the injected assets, token removed from the URL, the key in
    `localStorage`, the portal's Connected, the light home screen with two apps and "Connected to
    <PC>", Camera frames, the in-game app following, gyro, calibration, the photo blob, 2 s video,
    MP4 download, 403 without the session, Gallery grid + viewer + delete, the game's Disconnect →
    the page's Reconnect, Forget this phone → the game's in-game phone, card Not connected.
  - Screenshots: `phone_page_*.png`.
- **`tools/tests/phone_net.sh`**: host + client: the client sees the host's phone raised in portrait
  with a dark screen.
