# In-game phone and the companion page

Every player carries a phone (it is built in, outside the inventory). **X** takes it out over
whatever hotbar slot is selected and puts it away again; choosing a slot also puts it away.
Holding **right mouse** raises it into **camera mode**. Its screen is a live render target from its
lens.

The game also serves a **companion web page**. Open it on your real phone by scanning a QR code.
The page then:
- shows the in-game phone camera live;
- drives the in-game phone with the real phone's orientation;
- takes photos and videos, zooms and switches between the front and back cameras;
- lets you browse and save the gallery.

Everything stays on the local network: the game is the server, and nothing goes through the cloud.

## Phone flow rework: plan (written before the code)

Goal: equip = pairing, an always-raised phone, a HUD "simulated phone" panel, a Connect portal
with an in-game fallback, a small phone OS (home, Camera, Gallery), a white emissive screen.

1. **Flow state machine (pure, `phone_os.c`, unit tested).** `PhoneFlow`: `IDLE -> PAIRING`
   (portal) on equip; `PAIRING -> LOCAL` on decline, timeout (`phone_pair_timeout`) or server
   failure; `PAIRING / LOST -> PAIRED` on a device connect; `PAIRED -> LOST` on disconnect
   (portal with Reconnect); Cancel puts the phone away. Per session: re-equipping a paired or
   declined phone skips the portal. `phone_auto_pair 0` goes straight to `LOCAL`.
2. **Apps (`PhoneApp`, replicated).** `OFF, PORTAL, HOME, CAMERA, GALLERY` + gallery viewer
   sub-state. `assets/phone/apps.json` lists the home-screen apps (id, label, icon tile);
   `assets/phone/theme.json` holds the light phone-OS tokens and the HUD panel tokens. Layout
   rects are computed once in design px (640 x 1136) and shared by drawing and hit testing.
3. **Always focused.** Camera items always aim (`weapons.c`, no `BTN_AIM`), so the hold point
   stays in `phone_portrait` while equipped; P (pair) is removed, `phoneui` (Alt / pad d-pad
   right) toggles the cursor for the panel.
4. **One screen image, three consumers.** The CPU canvas (`phone_ui.c`) draws the OS over a white
   background (transparent only where the camera app shows the lens). `phone_view` composites it
   (camera apps over the lens image; other apps without any scene pass: `ui_only`) into the
   screen target (the 3D screen) and a new sRGB **panel** image with a rounded alpha mask (the HUD
   panel). Remote / spectated phones build their canvas from replicated state (no QR, no
   private photos), rendered locally.
5. **HUD panel** bottom-right above the item readout, ~22 % of the screen height
   (`hud_phone_size`, `hud_phone_corner`), bezel / radius / shadow from `assets/phone/theme.json`,
   `hud_opacity`; slides out + fades when a device connects, comes back on disconnect.
6. **Web page** = a mirrored HTML implementation (touch, native text, gestures and the browser's
   download / share need real DOM). It uses the same assets: the server injects
   `assets/phone/theme.json`, `apps.json` and the icon atlas (data URI) into the page, so both
   look the same. Page and game sync the current app both ways (`{"c":"app"}` / status `app`).
7. **Screen material.** Lit, near-black albedo (`0.04 x` the screen image) + emissive, so the glass
   gets real Fresnel reflections; emissive = `phone_screen_emissive` nits-like luminance turned
   into the displayed level `clamp(L x exposure, phone_screen_min, phone_screen_max)`: white but
   not blown out by day, a mild bloom at night. A small unshadowed point light at the screen
   socket (`phone_screen_light`). Off / not equipped: black.
8. **Replication.** `UserCmd.phone_app` + ANS_PHONE gains app and zoom (7 bytes). Protocol 19.
9. **Tests.** `test_phone` (flow, layout hit tests at 16:9 / 21:9 / 4:3, apps.json/theme.json,
   gallery delete, page injection); script tests (`phone_flow.cfg`: equip -> pairing in one tick,
   decline -> home, camera -> photo -> gallery, day / night screen levels, screenshots); a Python
   stdlib WebSocket device (`phone_flow_ws.py`) for connect / orientation / disconnect; the updated
   Playwright e2e; a two-instance LAN check.

| Area | Code | Data |
|---|---|---|
| Phone module: server lifecycle, page messages, orientation, capture, commands | `src/gameplay/phone/phone.c` | |
| Screen UI (CPU-drawn), HUD + pairing panel, the phone model | `src/gameplay/phone/phone_ui.c` | |
| Orientation math (pure) | `src/gameplay/phone/phone_orient.c` | |
| Stream rate control + JPEG (pure), the JPEG-over-WebSocket sink | `src/gameplay/phone/phone_stream.c`, `phone_stream_ws.c` | |
| Photos, sidecars, gallery (pure) / H.264 MP4 recording / QR codes | `phone_capture.c`, `phone_video.c`, `phone_qr.c` | |
| Wire protocol (shared with the page) | `src/gameplay/phone/phone_proto.h` | |
| HTTPS + WebSocket server, tokens, TLS + certificates | `src/net/http_ws/` | |
| Lens camera, screen target, light post, readback | `src/render/phone_view.c`, `shaders/post/phone_tonemap.comp` | |
| Hold point states, driven rotation | `src/player/holdpoint.c`, `src/anim/weapon_hold.c` | `assets/human/holdpoint.json` (`phone`, `phone_landscape`) |
| Hold styles | `src/anim/procedural.c` | `assets/human/controller.json` |
| Item | `src/gameplay/items.c` (`"camera"`, `"builtin"`) | `data/items.json` (`phone`) |
| Companion page | `web/phone/index.html` | |
| Settings tab | `src/ui/screens/scr_settings.c` (Phone) | |
| Tests | `tests/test_phone.c`, `tests/test_anim.c` (`test_phone_hold`), `tools/tests/phone_e2e.sh` | |

## Using it

| Key (default) | While holding the phone |
|---|---|
| **X** | take out / put away (from any slot) |
| **Right mouse** (hold) | raise the phone: camera mode |
| **Left mouse** | photo (camera mode) |
| **R** | start / stop recording (camera mode) |
| **Mouse wheel** | zoom 1x to 8x (camera mode; the wheel doesn't switch items then) |
| **T** | front (selfie) / back camera |
| **P** | pairing screen with the QR code (press again to close) |
| **J** | gallery on the phone's screen |

### Pairing

1. Take the phone out (X) and press **P**.
2. A QR code appears on the in-game phone's screen and, bigger, on a HUD panel. Both encode
   `https://<lan-ip>:47820/?t=<token>`. The address is also shown as text, in case your camera
   can't read the code.
3. Scan the code with the real phone. It must be on the same Wi-Fi.
4. The first time, the browser warns about the certificate (see the next section). Accept it.
5. Tap **Start**. On iOS, allow **Motion & Orientation** access when asked.
6. Hold the phone upright, facing the direction your character is looking. The first sample
   calibrates automatically. Later, use **Calibrate** in the page's menu, or double-tap the picture.

Pairing rules:
- The token is good for **one use** and **expires after 2 minutes** (`phone_token_ttl`).
- Using it creates a session key, which the page keeps in `sessionStorage`. Reloads (iOS forgets the
  motion permission on every reload) and media downloads use the session.
- Each player has one phone. Pairing another phone, or opening the page in a second tab, replaces the
  first one, which sees "Paired elsewhere".
- **Unpair** (on the page, or `phone_unpair`) revokes the session.
- Pairing again later works the same way: press P for a new code.

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
- **Other players' phones.** Each client renders the nearest other phone within
  `phone_remote_range` (4 m) itself, at `phone_remote_fps` (8 Hz, 270×480), from that phone's
  replicated transform. Nothing extra is sent over the network.

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
- The host copies them into the character state (`ANS_PHONE`, 5 bytes), only while in camera mode.
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
- **Screen.** A live render target:
  - the lens camera has its own FOV: 68° vertical at 1x; zoom narrows it as
    `2·atan(tan(fov/2)/zoom)`;
  - rendered at `phone_res_w × phone_res_h` (640×1136) at `phone_fps` (30 Hz);
  - opaque scene + characters + particles + rain, sky, then the **light post**
    (`phone_tonemap.comp`): the main view's auto exposure + `phone_exposure`, the user's tonemapper
    (ACES / PBR Neutral), vignette — no DoF, motion blur or bloom.

  The scene pass writes two images:
  - the clean sRGB camera image (stream, photos, video);
  - the screen image, with the **UI overlay** composited on top. The overlay is drawn on the CPU:
    status bar with in-game clock, battery and link, shutter / record / thumbnail, zoom chips and
    slider, SELFIE chip, pairing QR, gallery grid. It is uploaded only when it changes, at most twice
    a second.

  The quad is emissive, scaled by the current exposure so it reads like a lit display.
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
| `phone_server on|off` | start / stop the server (`phone_server` cvar: 0 off, 1 on, 2 automatic — starts the first time the phone comes out) |
| `phone_pair` | new pairing code: QR on the phone + HUD, URL printed to the console |
| `phone_unpair` | revoke the session |
| `phone_fake_orientation <pitch> <yaw> <roll>` / `off` | drive the phone without a device |
| `phone_stream_stats` | resolution, quality, fps, kbps, encode ms, RTT, glass-to-glass, server counters, orientation source |
| `phone_photo`, `phone_record`, `phone_zoom <x>`, `phone_selfie [0|1]`, `phone_landscape [0|1]`, `phone_screen camera|pair|gallery`, `phone_calibrate` | the same as the keys / page buttons |

Cvars:
- `phone_port`, `phone_tls`, `phone_bind`, `phone_pairing`, `phone_token_ttl`
- `phone_res_w`, `phone_res_h`, `phone_fps`, `phone_quality`
- `phone_smoothing`, `phone_sensitivity`, `phone_invert_yaw`, `phone_invert_pitch`,
  `phone_invert_roll`
- `phone_filter`, `phone_photo_hires`, `phone_exposure`, `phone_screen_brightness`
- `phone_video_kbps`, `phone_record_max`
- `phone_remote_range`, `phone_remote_fps`

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

- **`ctest -R phone`** (`tests/test_phone.c`, 385 checks):
  - QR encode → decode with quirc;
  - token auth: wrong, empty, prefix, expired at exactly 2 minutes, replayed, re-pair, unpair;
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
    unpair;
  - TLS: certificate generation, a handshake that verifies the IP SAN against the CA, the CA over
    the HTTP helper, CA reuse, a new server certificate for a new IP.
- **`ctest -R anim`** (`test_phone_hold`): the phone driven by random poses (some far outside the
  cone) in portrait and landscape.
  - Right hand on the phone: 0.04 mm portrait, about 6 mm at the landscape extremes (the arm is
    never stretched).
  - Left hand glued: 0.04 mm.
  - Item drift: 1e-7.
  - The camera-space rotation never leaves the reach cone.
- **`tools/tests/phone_e2e.sh`** (from `build/bin`): the game plus headless Chromium driving the
  real page.
  - Covers: HTTPS secure context, token removed from the URL, live frames decoded, gyro driving the
    in-game phone, calibration, the photo blob, 2 s video, gallery, MP4 download, 403 without the
    session, unpair.
  - Screenshots: `phone_page_*.png`.
