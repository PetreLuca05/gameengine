// End-to-end test of the phone companion page against a running game (docs/PHONE.md):
//   pair (URL from the game log) -> live feed frames -> orientation samples drive the in-game
//   phone -> shutter (photo + blob) -> 2 s video -> gallery + download -> unpair.
// Uses Playwright's Chromium with synthetic DeviceOrientation events (the game's certificate is
// self-signed: certificate errors are ignored, exactly what "visit this website" does on a phone).
//
//   node tools/tests/phone_e2e.mjs <game log> [screenshot dir]
// Normally started by tools/tests/phone_e2e.sh, which runs the game with phone_e2e.cfg.
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

const m = await waitLog(/phone_pair: open (https?:\/\/\S+)/, 240);
check(!!m, 'the game printed a pairing URL');
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
await page.click('#go');
// Session established: the one-time token left the address bar.
await page.waitForFunction(() => !!sessionStorage.getItem('dg_session'), null, { timeout: 30000 });
check(!page.url().includes('t='), 'pairing token removed from the URL after pairing');

// Orientation: hold upright (beta 90), then turn 30 deg left + tilt up 15 deg.
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
check(frames >= 12, 'live feed frames decoded on the page (' + frames + ')');
for (let i = 0; i < 25; i++) { await orient(30, 105, 0); await sleep(120); }
await page.screenshot({ path: shots + '/phone_page_feed.png' });
const st = await page.evaluate(() => status);
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

await page.click('#thumb');
await sleep(1500);
await page.screenshot({ path: shots + '/phone_page_gallery.png' });
await page.click('#gclose');

// Unpair: the page is told, the session stops working.
await page.click('#menu');
await page.click('#unpair');
await sleep(1500);
const sess = await page.evaluate(() => sessionStorage.getItem('dg_session'));
check(!sess, 'unpaired: session forgotten on the page');
await page.screenshot({ path: shots + '/phone_page_unpaired.png' });
await browser.close();
console.log(failed ? 'phone_e2e: ' + failed + ' FAILED' : 'phone_e2e: PASS');
process.exit(failed ? 1 : 0);
