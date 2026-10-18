// End-to-end test of the phone companion page against a running game (docs/PHONE.md):
//   pair (URL from the game log, printed when the Connect portal opens) -> the page's portal
//   "Connected" -> home screen (theme + icons injected by the game, "Connected to <PC>") -> Camera
//   (live feed frames, the in-game screen follows) -> orientation drives the in-game phone ->
//   shutter (photo + blob) -> 2 s video -> Gallery (grid, viewer, download, delete) -> the game's
//   Disconnect (the page offers Reconnect, no retry loop) -> Reconnect -> Forget this phone.
// Uses Playwright's Chromium with synthetic DeviceOrientation events (the game's certificate is
// self-signed: certificate errors are ignored, exactly what "visit this website" does on a phone).
//
//   node tools/tests/phone_e2e.mjs <game log> [screenshot dir]
// Normally started by tools/tests/phone_e2e.sh, which runs the game with phone_e2e.cfg.
// PLAYWRIGHT_PATH: an existing playwright module (nothing is installed by the test); CHROMIUM: a
// Chromium executable.
import { createRequire } from 'module';
import fs from 'fs';
const require = createRequire(import.meta.url);
let pw;
try { pw = require('playwright'); } catch { pw = require(process.env.PLAYWRIGHT_PATH || '/opt/node-tools/node_modules/playwright'); }

const log = process.argv[2];
const shots = process.argv[3] || '.';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failed++; };

async function waitLog(re, timeout_s) {
  for (let i = 0; i < timeout_s * 4; i++) {
    const txt = fs.existsSync(log) ? fs.readFileSync(log, 'utf8') : '';
    const m = txt.match(re);
    if (m) return m;
    await sleep(250);
  }
  return null;
}
const lastStatus = () => {
  const l = (fs.existsSync(log) ? fs.readFileSync(log, 'utf8') : '').split('\n').filter((x) => x.includes('phone_status:'));
  return l.length ? l[l.length - 1] : '';
};

const m = await waitLog(/phone_pair: open (https?:\/\/\S+)/, 240);
check(!!m, 'the Connect portal printed a pairing URL');
if (!m) process.exit(1);
const url = m[1];
console.log('url', url);

const browser = await pw.chromium.launch({ executablePath: process.env.CHROMIUM || undefined, args: ['--ignore-certificate-errors'] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, hasTouch: true, isMobile: true });
const page = await ctx.newPage();
page.on('dialog', (d) => d.accept());
page.on('console', (msg) => { if (msg.type() === 'error') console.log('page error:', msg.text()); });
await page.goto(url);
check(await page.evaluate(() => window.isSecureContext), 'secure context (HTTPS: motion sensors allowed)');
check(await page.evaluate(() => !!window.DG_THEME && !!window.DG_APPS && String(window.DG_ICONS).startsWith('data:image/png')), 'the game injected theme, apps and icon atlas');
// Session established: the one-time token left the address bar; the portal says Connected.
await page.waitForFunction(() => !!localStorage.getItem('dg_session'), null, { timeout: 30000 });
check(true, 'session key kept in localStorage (survives the page and the game: the stored device)');
check(!page.url().includes('t='), 'pairing token removed from the URL after pairing');
await page.waitForFunction(() => document.getElementById('ptitle').textContent === 'Connected', null, { timeout: 10000 }).catch(() => {});
check((await page.textContent('#ptitle')) === 'Connected', 'Connect portal: Connected');
await page.screenshot({ path: shots + '/phone_page_portal.png' });
await page.click('#go');
await sleep(800);
check(await page.isVisible('#s-home'), 'home screen after the permission step');
const bg = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
check(bg === 'rgb(255, 255, 255)', 'light theme from assets/phone/theme.json (' + bg + ')');
check((await page.$$('#apps .app')).length === 2, 'home grid: Camera and Gallery');
await page.waitForFunction(() => /Connected to /.test(document.getElementById('pcname').textContent), null, { timeout: 10000 }).catch(() => {});
check(/Connected to /.test(await page.textContent('#pcname')), 'banner: ' + (await page.textContent('#pcname')));
check((await page.$$('#apps .app.off')).length === 0, 'in a match: the apps are enabled');
await page.waitForFunction(() => /\d\d:\d\d/.test(document.getElementById('hclock').textContent), null, { timeout: 10000 }).catch(() => {});
await page.screenshot({ path: shots + '/phone_page_home.png' });

// Camera: the page sends the app, the game streams frames and its screen follows.
await page.click('#app-camera');
async function orient(a, b, g) {
  await page.evaluate(([a, b, g]) => window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', { alpha: a, beta: b, gamma: g })), [a, b, g]);
}
const t0 = Date.now();
let frames = 0;
while (Date.now() - t0 < 60000) {
  await orient(0, 90, 0);
  frames = await page.evaluate(() => (typeof lastFrameId === 'number' ? lastFrameId : 0));
  if (frames >= 12) break;
  await sleep(200);
}
check(frames >= 12, 'camera app: live feed frames decoded on the page (' + frames + ')');
for (let i = 0; i < 25; i++) { await orient(30, 105, 0); await sleep(120); }
await page.screenshot({ path: shots + '/phone_page_feed.png' });
const st = await page.evaluate(() => status);
check(st.app === 'camera', 'the in-game phone is on the camera app too');
check(st.driven === true, 'the in-game phone is driven by the page orientation');
check(st.calibrated === true, 'calibrated on the first sample');

// Photo: the game saves it and pushes the JPEG back.
await page.click('#shutter');
await page.waitForFunction(() => !!lastPhoto, null, { timeout: 60000 }).catch(() => {});
const photoBytes = await page.evaluate(async () => (lastPhoto ? (await lastPhoto.blob.arrayBuffer()).byteLength : 0));
check(photoBytes > 10000, 'photo blob received for save-to-phone (' + photoBytes + ' bytes)');

// Video: 2 s.
await page.click('#record');
await page.waitForFunction(() => status.rec === true, null, { timeout: 30000 }).catch(() => {});
check((await page.evaluate(() => status.rec)) === true, 'recording started');
await sleep(2500);
await page.click('#record');
await page.waitForFunction(() => status.rec === false && gallery.some((g) => g.kind === 'video'), null, { timeout: 60000 }).catch(() => {});
const items = await page.evaluate(() => gallery);
const vid = items.find((g) => g.kind === 'video'), pho = items.find((g) => g.kind === 'photo');
check(!!vid && vid.seconds >= 1.5, 'video in the gallery (' + (vid ? vid.seconds + ' s' : 'none') + ')');
check(!!pho, 'photo in the gallery');

// Downloads with the session (the page's own links).
const dl = await page.evaluate(async (it) => {
  const r = await fetch(it.url + '?s=' + encodeURIComponent(sessionStorage.getItem('dg_session')) + '&dl=1');
  const b = new Uint8Array(await r.arrayBuffer());
  return { status: r.status, type: r.headers.get('content-type'), n: b.length, head: Array.from(b.slice(4, 8)) };
}, vid || {});
check(dl.status === 200 && dl.type === 'video/mp4' && String.fromCharCode(...dl.head) === 'ftyp', 'video download is an MP4 (' + dl.n + ' bytes)');
const noauth = await page.evaluate(async (it) => (await fetch(it.url)).status, pho || {});
check(noauth === 403, 'media without the session is refused');

// Home -> Gallery (the home bar, then the app): grid, viewer, delete.
await page.click('#homebar');
await sleep(300);
await page.click('#app-gallery');
await page.waitForFunction(() => document.querySelectorAll('#grid .cell').length > 0, null, { timeout: 10000 }).catch(() => {});
await sleep(1500);
check((await page.$$('#grid .cell')).length >= 2, 'gallery grid shows the photo and the video');
await sleep(1200);
const gst = await page.evaluate(() => status);
check(gst.app === 'gallery', 'the in-game phone is on the gallery too');
await page.screenshot({ path: shots + '/phone_page_gallery.png' });
await page.click('#grid .cell');
await sleep(800);
check(await page.isVisible('#viewer'), 'viewer opens');
await page.screenshot({ path: shots + '/phone_page_viewer.png' });
const before = (await page.evaluate(() => gallery)).length;
const delId = await page.evaluate(() => gallery[viewerIdx].id);
await page.click('#vdel');
await page.waitForFunction((n) => gallery.length === n - 1, before, { timeout: 15000 }).catch(() => {});
check((await page.evaluate(() => gallery.length)) === before - 1 && !(await page.evaluate((id) => gallery.some((g) => g.id === id), delId)), 'delete removes the item in the game');

// The game's Disconnect (Phone card): close 4002, the page offers Reconnect (no retry loop).
await page.click('#homebar');
await page.waitForFunction(() => document.getElementById('ptitle').textContent === 'Disconnected', null, { timeout: 180000 }).catch(() => {});
check((await page.textContent('#ptitle')) === 'Disconnected' && (await page.isVisible('#go')) && (await page.textContent('#go')) === 'Reconnect', 'game Disconnect: the page shows Reconnect');
await page.screenshot({ path: shots + '/phone_page_disconnected.png' });
await page.click('#go');
await page.waitForFunction(() => document.getElementById('ptitle').textContent === 'Connected', null, { timeout: 30000 }).catch(() => {});
check((await page.textContent('#ptitle')) === 'Connected', 'Reconnect: connected again with the stored session');
await page.click('#go');
await sleep(800);
// Forget this phone: the page is told, the session stops working; the game falls back to the in-game phone.
await page.click('#app-camera');
await sleep(300);
await page.click('#menu');
await page.click('#unpair');
await sleep(1500);
const sess = await page.evaluate(() => localStorage.getItem('dg_session') || sessionStorage.getItem('dg_session'));
check(!sess, 'forgotten: session removed on the page');
await page.screenshot({ path: shots + '/phone_page_unpaired.png' });
await browser.close();
const lost = await waitLog(/phone_status: flow=local app=\w+ panel=1/, 120);
check(!!lost, 'the game: device gone -> the in-game phone (HUD panel)');
const card = await waitLog(/phone_card: card=not_connected mode=unset[^\n]*stored=0/, 60);
check(!!card, 'the game: card Not connected, nothing stored');
console.log('last status:', lastStatus());
console.log(failed ? 'phone_e2e: ' + failed + ' FAILED' : 'phone_e2e: PASS');
process.exit(failed ? 1 : 0);
