#!/usr/bin/env python3
"""Phone pairing in the menus with a simulated device (docs/PHONE.md "Phone flow", "Tests"): Python
stdlib only (ssl + socket).

Run 1 (tools/tests/phone_ws.cfg, --menu): the server is up with the main menu (auto-pair); the
Player tab's Phone card opens the Connect portal (a modal), the simulated page pairs with the
one-time token from the log: card Connected, the page's status says ctx "menu". A LAN lobby shows
the phone flag; in the match, equipping goes straight to the device (no portal, no HUD panel), the
page opens Camera (live frames), takes a photo, opens Gallery. The socket drops: the game falls
back to the in-game phone with a toast; the page resumes with its session key: real-phone mode
again. The pause menu's card opens the portal too.
Run 2 (tools/tests/phone_ws_relaunch.cfg): the stored device's session is restored with the
server; the page reconnects without a new code (resumed); Forget device deletes it.

    cd build/bin && python ../../tools/tests/phone_ws.py [--exe deadground.exe] [--port 27392] [--phone-port 47392]
"""
import argparse, base64, json, os, re, socket, ssl, struct, subprocess, sys, time, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        fails += 1


def tls_connect(host, port):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE   # the game's own local CA ("visit this website" on a phone)
    raw = socket.create_connection((host, port), timeout=10)
    return ctx.wrap_socket(raw, server_hostname=host)


def http_get(host, port, path):
    s = tls_connect(host, port)
    s.sendall(("GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n" % (path, host, port)).encode())
    data = b""
    while True:
        try:
            b = s.recv(65536)
        except socket.timeout:
            break
        if not b:
            break
        data += b
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    code = int(head.split(b" ")[1]) if head else 0
    hdrs = {}
    for line in head.split(b"\r\n")[1:]:
        k, _, v = line.partition(b":")
        hdrs[k.strip().lower().decode()] = v.strip().decode()
    if hdrs.get("transfer-encoding") == "chunked":
        out, rest = b"", body
        while rest:
            ln, _, rest = rest.partition(b"\r\n")
            n = int(ln, 16)
            if n == 0:
                break
            out, rest = out + rest[:n], rest[n + 2:]
        body = out
    return code, hdrs, body


class Ws:
    def __init__(self, host, port, path):
        self.s = tls_connect(host, port)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
                        % (path, host, port, key)).encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.s.recv(4096)
        head, _, self.buf = resp.partition(b"\r\n\r\n")
        self.ok = b" 101 " in head.split(b"\r\n")[0]
        self.s.settimeout(0.05)

    def send(self, op, payload):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | op])
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def text(self, obj):
        self.send(1, json.dumps(obj).encode())

    def poll(self):
        """Returns the complete messages received so far: [(opcode, payload)]."""
        try:
            while True:
                b = self.s.recv(65536)
                if not b:
                    break
                self.buf += b
        except (socket.timeout, ssl.SSLWantReadError, BlockingIOError):
            pass
        out = []
        while len(self.buf) >= 2:
            b0, b1 = self.buf[0], self.buf[1]
            n, off = b1 & 0x7F, 2
            if n == 126:
                if len(self.buf) < 4:
                    break
                n, off = struct.unpack(">H", self.buf[2:4])[0], 4
            elif n == 127:
                if len(self.buf) < 10:
                    break
                n, off = struct.unpack(">Q", self.buf[2:10])[0], 10
            if len(self.buf) < off + n:
                break
            op, payload = b0 & 0x0F, self.buf[off:off + n]
            self.buf = self.buf[off + n:]
            if op == 9:
                self.send(10, payload)   # pong
                continue
            out.append((op, payload))
        return out

    def close(self):
        try:
            self.send(8, struct.pack(">H", 1000))
            time.sleep(0.2)
        finally:
            self.s.close()


def orient_msg(seq, q):
    return struct.pack("<BBHI4h", 1, 0, seq & 0xFFFF, int(time.time() * 1000) & 0xFFFFFFFF, *[int(max(-1, min(1, c)) * 32767) for c in q])


def upright(yaw_deg=0.0):
    # device -> earth for a phone held upright facing north (beta 90), turned by yaw about up.
    import math
    def qmul(a, b):
        return (a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1], a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
                a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3], a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2])
    def ax(x, y, z, d):
        h = math.radians(d) / 2
        return (x * math.sin(h), y * math.sin(h), z * math.sin(h), math.cos(h))
    return qmul(qmul(ax(0, 0, 1, yaw_deg), ax(1, 0, 0, 90)), ax(0, 1, 0, 0))


def log_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_log(path, pattern, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        m = re.search(pattern, log_text(path))
        if m:
            return m
        time.sleep(0.25)
    return None


def statuses(path):
    return [dict(re.findall(r"(\w+)=(\"[^\"]*\"|\S+)", l)) for l in log_text(path).splitlines() if "phone_status:" in l]


def cards(path):
    return [dict(re.findall(r"(\w+)=(\"[^\"]*\"|\S+)", l)) for l in log_text(path).splitlines() if "phone_card:" in l]


def run_game(a, cfg, log):
    if os.path.exists(log):
        os.remove(log)
    return subprocess.Popen([a.exe, "--test", "--menu", "--nosound", "--log", log, "+set", "net_port", str(a.port), "+set", "phone_port", str(a.phone_port),
                             "--script", os.path.join(HERE, cfg)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Page:
    """The companion page's protocol side: messages, frames, orientation samples."""

    def __init__(self, ws):
        self.ws, self.session, self.status, self.frames, self.media, self.photo_blob, self.gallery, self.close_code = ws, None, None, 0, None, 0, None, None

    def pump(self, dt, orient=True):
        t0 = time.time()
        while time.time() - t0 < dt:
            for op, p in self.ws.poll():
                if op == 1:
                    j = json.loads(p.decode())
                    t = j.get("t")
                    if t == "session":
                        self.session = j.get("s")
                    elif t == "status":
                        self.status = j
                    elif t == "media":
                        self.media = j["item"]
                    elif t == "gallery":
                        self.gallery = j.get("items", [])
                elif op == 2 and p:
                    if p[0] == 16:
                        self.frames += 1
                        self.ws.text({"c": "ack", "f": struct.unpack("<I", p[8:12])[0]})
                    elif p[0] == 17:
                        self.photo_blob = len(p) - 8
                elif op == 8:
                    self.close_code = struct.unpack(">H", p[:2])[0] if len(p) >= 2 else 0
            if orient:
                try:
                    self.ws.send(2, orient_msg(int(time.time() * 60), upright(10)))
                except OSError:
                    pass
            time.sleep(1 / 60)

    def wait_status(self, cond, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.pump(0.25)
            if self.status and cond(self.status):
                return True
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.abspath("deadground.exe" if os.name == "nt" else "deadground"))
    ap.add_argument("--port", type=int, default=27392)
    ap.add_argument("--phone-port", type=int, default=47392)
    ap.add_argument("--log", default="phone_ws.log")
    a = ap.parse_args()
    log2 = a.log.replace(".log", "_relaunch.log")
    game = run_game(a, "phone_ws.cfg", a.log)
    session = None
    try:
        # ---- the main menu: card, portal, pairing
        m = wait_log(a.log, r"phone_pair: open (https?://\S+)", 600)
        check(bool(m), "the Connect portal issued a pairing code (from the main menu)")
        if not m:
            return
        cd = cards(a.log)
        check(bool(cd) and cd[0].get("card") == "not_connected" and cd[0].get("server") == "1" and cd[0].get("ctx") == "menu",
              "main menu: the server is up (auto-pair), card Not connected (%s)" % (cd[0] if cd else None))
        # (the script logs the portal's state a few frames later: pair only after that)
        m2 = wait_log(a.log, r"phone_status: flow=pairing[^\n]*", 600)
        check(bool(m2) and "token=1" in m2.group(0) and "portal=1" in m2.group(0) and "screen=phone" in m2.group(0), "the portal is a modal (screen phone) with a code")
        check(bool(wait_log(a.log, r"phone_card: card=waiting", 120)), "card: Waiting...")
        url = urllib.parse.urlparse(m.group(1))
        host, port, token = url.hostname, url.port, urllib.parse.parse_qs(url.query)["t"][0]
        code, hdrs, page = http_get(host, port, "/")
        check(code == 200 and b"window.DG_THEME={" in page and b"window.DG_APPS={" in page and b'DG_ICONS="data:image/png;base64,' in page
              and b"/*DG_PHONE_DATA*/" not in page, "the page carries the phone OS assets (theme, apps, icon atlas)")
        ws = Ws(host, port, "/ws?t=" + token)
        check(ws.ok, "WebSocket upgrade with the pairing token")
        pg = Page(ws)
        ws.text({"c": "hello", "v": 1, "ua": "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) AppleWebKit/605.1.15 Version/17.4 Mobile/15E148 Safari/604.1"})
        check(pg.wait_status(lambda s: s.get("ctx") == "menu", 120), "status in the menu: ctx menu (the companion screen), pc '%s'" % ((pg.status or {}).get("pc")))
        session = pg.session
        check(bool(session), "session key issued")
        check(code == 200 and http_get(host, port, "/ping?s=" + urllib.parse.quote(session or ""))[0] == 200, "/ping with the session: 200")
        m = wait_log(a.log, r"phone_card: card=connected[^\n]*", 600)
        check(bool(m) and 'device="iPhone \xc2\xb7 Safari"'.encode("latin-1").decode("utf-8") in m.group(0) and "ctx=menu" in m.group(0), "card: Connected (iPhone \u00b7 Safari) in the menu")
        check(bool(wait_log(a.log, r"mm_dump: [^\n]*phone 'Connected \(iPhone", 300)), "the main menu footer shows the phone status")
        check(bool(wait_log(a.log, r"phone_status: flow=paired[^\n]*portal=0 screen=mainmenu", 300)), "the portal closed by itself after Connected")
        # ---- the match
        check(pg.wait_status(lambda s: s.get("ctx") == "match", 900), "entering the match: the page's ctx becomes match")
        m = wait_log(a.log, r"lobby: [^\n]*\n\s*#\d+ [^\n]* phone", 600)
        check(bool(m), "the lobby member list carries the phone flag")
        check(pg.wait_status(lambda s: s.get("holding") is True, 900), "the phone comes out in the match")
        check(pg.status.get("flow") == "paired" and pg.status.get("app") == "home", "equip with the device: paired, home (%s / %s)" % (pg.status.get("flow"), pg.status.get("app")))
        ws.text({"c": "app", "v": "camera"})
        pg.pump(8.0)
        check(pg.frames > 0, "camera app: live frames streamed to the page (%d)" % pg.frames)
        check(bool(pg.status) and pg.status.get("driven") is True, "orientation samples drive the in-game phone")
        ws.text({"c": "shutter"})
        t0 = time.time()
        while time.time() - t0 < 120 and not pg.media:
            pg.pump(0.5)
        check(bool(pg.media) and pg.media.get("kind") == "photo", "photo taken from the page (media message)")
        check(pg.photo_blob > 10000, "photo JPEG pushed for save-to-phone (%d bytes)" % pg.photo_blob)
        ws.text({"c": "app", "v": "gallery"})
        ws.text({"c": "gallery"})
        pg.pump(4.0)
        listed = bool(pg.gallery) and pg.media and pg.gallery[0].get("id") == pg.media.get("id")
        check(listed, "gallery lists the new photo first")
        if listed:
            code, hdrs, body = http_get(host, port, pg.gallery[0]["url"] + "?s=" + urllib.parse.quote(session))
            check(code == 200 and hdrs.get("content-type") == "image/jpeg" and body[:2] == b"\xff\xd8", "photo downloads with the session")
        # ---- the device drops mid-match, then the page resumes
        ws.s.close()
        m = wait_log(a.log, r"phone_wait: local (reached|TIMEOUT)", 600)
        check(bool(m) and m.group(1) == "reached", "device dropped: the in-game phone")
        check(bool(wait_log(a.log, r"Phone disconnected \S+ using in-game phone", 30)), "toast: Phone disconnected - using in-game phone")
        time.sleep(3)
        check(http_get(host, port, "/ping?s=" + urllib.parse.quote(session))[0] == 200, "/ping after the drop: the session is still good")
        ws = Ws(host, port, "/ws?s=" + urllib.parse.quote(session))
        check(ws.ok, "the page resumes with its session key")
        pg = Page(ws)
        ws.text({"c": "hello", "v": 1, "ua": "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) AppleWebKit/605.1.15 Version/17.4 Mobile/15E148 Safari/604.1"})
        check(pg.wait_status(lambda s: s.get("flow") == "paired", 300), "resumed: real-phone mode again")
        while game.poll() is None:
            pg.pump(0.5)
        st = statuses(a.log)
        match = [s for s in st if s.get("raised") == "1" or s.get("app") in ("camera", "gallery")]
        check(bool(match) and all(s.get("screen") != "phone" for s in match[:2]), "the in-match equip never opened the portal")
        paired = [s for s in st if s.get("flow") == "paired"]
        check(bool(paired) and all(s.get("panel") == "0" for s in paired), "paired: no HUD panel")
        check(any(s.get("driven") == "1" for s in paired), "paired: driven by the device orientation")
        check(any(s.get("app") == "camera" for s in paired) and any(s.get("app") == "gallery" for s in paired), "the in-game screen mirrors the page's app")
        local = [s for s in st if s.get("flow") == "local"]
        check(bool(local) and local[0].get("panel") == "1" and local[0].get("reason") == '"Phone disconnected"', "after the drop: the HUD panel is back")
        check(any(s.get("screen") == "phone" and s.get("portal") == "1" for s in st[-3:]), "the pause menu's card opens the portal")
        check(st and st[-1].get("portal") == "0" and st[-1].get("flow") == "paired", "portal closed, still paired")
        check(os.path.exists("phone_ws_pause.png"), "screenshot of the pause menu card")
    finally:
        if game.poll() is None:
            game.kill()
    if not session:
        print("phone_ws: " + ("PASS" if not fails else "%d FAILED" % fails))
        return
    # ---- the next launch: the stored device resumes (the page left open), then Forget device
    game = run_game(a, "phone_ws_relaunch.cfg", log2)
    try:
        m = wait_log(log2, r"phone: waiting for the stored device \(([^)]*)\)", 600)
        check(bool(m) and m.group(1).startswith("iPhone"), "relaunch: the stored device is restored (%s)" % (m.group(1) if m else None))
        ws = None
        t0 = time.time()
        while time.time() - t0 < 120:
            try:
                ws = Ws("127.0.0.1" if host == "localhost" else host, port, "/ws?s=" + urllib.parse.quote(session))
                if ws.ok:
                    break
            except OSError:
                pass
            time.sleep(1)
        check(bool(ws) and ws.ok, "relaunch: the page reconnects with its stored key (no new code)")
        if ws and ws.ok:
            pg = Page(ws)
            ws.text({"c": "hello", "v": 1, "ua": "phone_ws.py"})
            while game.poll() is None and pg.close_code is None:
                pg.pump(0.5, orient=False)
            check(pg.close_code == 4001, "Forget device closes the page with 4001 (%s)" % pg.close_code)
        game.wait(600)
        cd = cards(log2)
        check(any(c.get("card") == "connected" and c.get("stored") == "1" for c in cd), "relaunch: card Connected, stored")
        check(bool(cd) and cd[-1].get("card") == "not_connected" and cd[-1].get("stored") == "0", "Forget device: not connected, nothing stored")
    finally:
        if game.poll() is None:
            game.kill()
    print("phone_ws: " + ("PASS" if not fails else "%d FAILED" % fails))


if __name__ == "__main__":
    main()
    sys.exit(1 if fails else 0)
