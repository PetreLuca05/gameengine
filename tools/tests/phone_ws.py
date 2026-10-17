#!/usr/bin/env python3
"""Phone flow with a simulated device (docs/PHONE.md "Tests"): Python stdlib only (ssl + socket).

Runs the game with tools/tests/phone_ws.cfg, waits for the pairing URL in its log, then acts as
the companion page: fetches the page (asset injection), pairs over a WebSocket with the one-time
token, streams orientation samples, opens Camera (live frames arrive), takes a photo (media
message + JPEG blob), opens Gallery (the photo is listed and downloads with the session), and
disconnects. The game's phone_status lines must show: portal + panel on equip, paired + panel
gone + driven while connected, camera / gallery mirrored, lost + portal + panel back at the end.

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.abspath("deadground.exe" if os.name == "nt" else "deadground"))
    ap.add_argument("--port", type=int, default=27392)
    ap.add_argument("--phone-port", type=int, default=47392)
    ap.add_argument("--log", default="phone_ws.log")
    a = ap.parse_args()
    if os.path.exists(a.log):
        os.remove(a.log)
    game = subprocess.Popen([a.exe, "--test", "--nosound", "--log", a.log, "+set", "net_port", str(a.port), "+set", "phone_port", str(a.phone_port),
                             "--script", os.path.join(HERE, "phone_ws.cfg")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        m = wait_log(a.log, r"phone_pair: open (https?://\S+)", 240)
        check(bool(m), "the game issued a pairing code on equip")
        if not m:
            return
        url = urllib.parse.urlparse(m.group(1))
        host, port, token = url.hostname, url.port, urllib.parse.parse_qs(url.query)["t"][0]
        st = statuses(a.log)
        check(any(s.get("flow") == "pairing" and s.get("app") == "portal" and s.get("server") == "1" and s.get("token") == "1" for s in st),
              "one frame after equip: server up, pairing, portal")
        code, hdrs, page = http_get(host, port, "/")
        check(code == 200 and b"window.DG_THEME={" in page and b"window.DG_APPS={" in page and b'DG_ICONS="data:image/png;base64,' in page
              and b"/*DG_PHONE_DATA*/" not in page, "the page carries the phone OS assets (theme, apps, icon atlas)")
        ws = Ws(host, port, "/ws?t=" + token)
        check(ws.ok, "WebSocket upgrade with the pairing token")
        session, got_status, frames, media, photo_blob, gallery = None, None, 0, None, 0, None

        def pump(dt):
            nonlocal session, got_status, frames, media, photo_blob, gallery
            t0 = time.time()
            while time.time() - t0 < dt:
                for op, p in ws.poll():
                    if op == 1:
                        j = json.loads(p.decode())
                        if j.get("t") == "session":
                            session = j.get("s")
                        elif j.get("t") == "status":
                            got_status = j
                        elif j.get("t") == "media":
                            media = j["item"]
                        elif j.get("t") == "gallery":
                            gallery = j.get("items", [])
                    elif op == 2 and p:
                        if p[0] == 16:
                            frames += 1
                            ws.text({"c": "ack", "f": struct.unpack("<I", p[8:12])[0]})
                        elif p[0] == 17:
                            photo_blob = len(p) - 8
                ws.send(2, orient_msg(int(time.time() * 60), upright(10)))
                time.sleep(1 / 60)

        ws.text({"c": "hello", "v": 1, "ua": "phone_ws.py"})
        pump(3.0)
        check(bool(session), "session key issued")
        check(bool(got_status) and got_status.get("flow") == "paired" and got_status.get("app") == "home",
              "status: paired, home screen (%s / %s)" % ((got_status or {}).get("flow"), (got_status or {}).get("app")))
        ws.text({"c": "app", "v": "camera"})
        pump(6.0)
        check(frames > 0, "camera app: live frames streamed to the page (%d)" % frames)
        check(bool(got_status) and got_status.get("driven") is True, "orientation samples drive the in-game phone")
        ws.text({"c": "shutter"})
        t0 = time.time()
        while time.time() - t0 < 30 and not media:
            pump(0.5)
        check(bool(media) and media.get("kind") == "photo", "photo taken from the page (media message)")
        check(photo_blob > 10000, "photo JPEG pushed for save-to-phone (%d bytes)" % photo_blob)
        ws.text({"c": "app", "v": "gallery"})
        ws.text({"c": "gallery"})
        pump(3.0)
        listed = bool(gallery) and media and gallery[0].get("id") == media.get("id")
        check(listed, "gallery lists the new photo first")
        if listed:
            code, hdrs, body = http_get(host, port, gallery[0]["url"] + "?s=" + urllib.parse.quote(session))
            check(code == 200 and hdrs.get("content-type") == "image/jpeg" and body[:2] == b"\xff\xd8", "photo downloads with the session")
        pump(1.0)
        ws.close()
        m = wait_log(a.log, r"phone_wait: lost (reached|TIMEOUT)", 120)
        check(bool(m) and m.group(1) == "reached", "device disconnect -> lost")
        game.wait(180)
        st = statuses(a.log)
        paired = [s for s in st if s.get("flow") == "paired"]
        check(bool(paired) and all(s.get("panel") == "0" for s in paired), "paired: the HUD panel is removed")
        check(any(s.get("driven") == "1" for s in paired), "paired: driven by the device orientation")
        check(any(s.get("app") == "camera" for s in paired) and any(s.get("app") == "gallery" for s in paired), "the in-game screen mirrors the page's app")
        last = st[-1] if st else {}
        check(last.get("flow") == "lost" and last.get("app") == "portal" and last.get("panel") == "1", "after the disconnect: panel back on the portal (Reconnect)")
    finally:
        if game.poll() is None:
            game.kill()
    print("phone_ws: " + ("PASS" if not fails else "%d FAILED" % fails))


if __name__ == "__main__":
    main()
    sys.exit(1 if fails else 0)
