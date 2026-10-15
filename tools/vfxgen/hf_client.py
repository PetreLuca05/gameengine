"""Higgsfield API client for vfxgen (texture generation only; the game never calls the API).

Auth: `Authorization: Key <id>:<secret>` from OPEN_HIGGSFIELD_READ_WRITE_TOKEN in the project
`.env` (git-ignored). The token value is never printed or logged.

Flow per request:  POST /estimate/<model>  ->  POST /<model> (Idempotency-Key)  ->  poll status_url
until completed / failed / nsfw / canceled  ->  download images[].url or video.url.
Failed and nsfw requests are not billed. Outputs are kept by the service for 7 days, so the
downloaded files are cached locally (tools/vfxgen/cache, git-ignored).

Every call is appended to tools/vfxgen/logs/api_log.jsonl (method, url, status, request id,
estimated / charged cost, recipe) - without any credentials.
"""
import hashlib
import json
import os
import time
import uuid

import requests

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOG_DIR = os.path.join(os.path.dirname(__file__), "logs")
DEFAULT_BASES = ["https://platform.higgsfield.ai", "https://api.higgsfield.ai"]
TERMINAL = ("completed", "failed", "nsfw", "canceled", "cancelled")


class HiggsfieldError(RuntimeError):
    pass


def load_env(path=None):
    """Reads KEY=VALUE pairs from .env without exporting them to child processes."""
    path = path or os.path.join(ROOT, ".env")
    env = {}
    if not os.path.exists(path):
        return env
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"').strip("'")
    return env


def _redact(obj):
    if isinstance(obj, dict):
        return {k: ("<redacted>" if "auth" in k.lower() or "token" in k.lower() or "key" == k.lower() else _redact(v)) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_redact(v) for v in obj]
    return obj


class Client:
    def __init__(self, base_url=None, token=None, log_name="api_log.jsonl", timeout=60):
        env = load_env()
        self.token = token or os.environ.get("OPEN_HIGGSFIELD_READ_WRITE_TOKEN") or env.get("OPEN_HIGGSFIELD_READ_WRITE_TOKEN")
        if not self.token:
            raise HiggsfieldError("OPEN_HIGGSFIELD_READ_WRITE_TOKEN missing (.env at the project root, see .env.example)")
        if ":" not in self.token:
            raise HiggsfieldError("token must be '<key id>:<key secret>'")
        configured = base_url or env.get("HF_API_BASE_URL")
        self.bases = ([configured.rstrip("/")] if configured else []) + [b for b in DEFAULT_BASES if b != (configured or "").rstrip("/")]
        self.base = self.bases[0]
        self.timeout = timeout
        os.makedirs(LOG_DIR, exist_ok=True)
        self.log_path = os.path.join(LOG_DIR, log_name)
        self.session = requests.Session()

    # -------------------------------------------------------------- plumbing

    def _headers(self, idem=None):
        h = {"Authorization": "Key " + self.token, "Content-Type": "application/json", "Accept": "application/json"}
        if idem:
            h["Idempotency-Key"] = idem
        return h

    def _log(self, **entry):
        entry["time"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        with open(self.log_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(_redact(entry)) + "\n")

    def _request(self, method, path_or_url, body=None, idem=None, recipe=None, retries=4):
        url = path_or_url if path_or_url.startswith("http") else self.base + path_or_url
        delay = 2.0
        for attempt in range(retries + 1):
            try:
                r = self.session.request(method, url, headers=self._headers(idem), json=body, timeout=self.timeout)
            except requests.RequestException as e:
                self._log(method=method, url=url, error=str(e), recipe=recipe, attempt=attempt)
                if attempt == retries:
                    raise HiggsfieldError(f"{method} {url}: {e}")
                time.sleep(delay)
                delay *= 2
                continue
            try:
                data = r.json()
            except ValueError:
                data = {"_text": r.text[:300]}
            self._log(method=method, url=url, status=r.status_code, recipe=recipe, attempt=attempt,
                      request_id=data.get("request_id") if isinstance(data, dict) else None,
                      response=_summary(data))
            # Concurrency limit / transient errors: back off and retry (same idempotency key).
            if r.status_code in (429, 500, 502, 503, 504) or (r.status_code == 400 and "concurren" in json.dumps(data).lower()):
                if attempt < retries:
                    time.sleep(delay)
                    delay *= 2
                    continue
            if r.status_code >= 400:
                raise HiggsfieldError(f"{method} {url} -> HTTP {r.status_code}: {json.dumps(data)[:400]}")
            return data
        raise HiggsfieldError(f"{method} {url}: retries exhausted")

    def _probe_base(self, model, params):
        """The configured base may be a web host; fall back to the documented API host."""
        last = None
        for b in self.bases:
            self.base = b
            try:
                return self._request("POST", f"/estimate/{model}", params, retries=1, recipe="estimate-probe")
            except HiggsfieldError as e:
                last = e
                msg = str(e)
                if "HTTP 401" in msg or "HTTP 403" in msg:
                    raise
        raise last

    # -------------------------------------------------------------- API

    def estimate(self, model, params, recipe=None):
        """Returns {'credits': .., 'usd': ..} (or whatever the service reports)."""
        if not getattr(self, "_probed", False):
            data = self._probe_base(model, params)
            self._probed = True
        else:
            data = self._request("POST", f"/estimate/{model}", params, recipe=recipe)
        return data

    def submit(self, model, params, idem, recipe=None):
        return self._request("POST", f"/{model}", params, idem=idem, recipe=recipe)

    def status(self, status_url, recipe=None):
        return self._request("GET", status_url, recipe=recipe)

    def cancel(self, cancel_url, recipe=None):
        return self._request("POST", cancel_url, recipe=recipe, retries=0)

    def wait(self, sub, recipe=None, timeout=1200, poll=5.0):
        url = sub.get("status_url") or f"{self.base}/requests/{sub['request_id']}/status"
        t0 = time.time()
        while True:
            st = self.status(url, recipe=recipe)
            s = str(st.get("status", "")).lower()
            if s in TERMINAL:
                return st
            if time.time() - t0 > timeout:
                if sub.get("cancel_url"):
                    try:
                        self.cancel(sub["cancel_url"], recipe=recipe)
                    except HiggsfieldError:
                        pass
                raise HiggsfieldError(f"request {sub.get('request_id')} timed out")
            time.sleep(poll)
            poll = min(poll * 1.3, 20.0)

    def download(self, url, dst, recipe=None):
        r = self.session.get(url, timeout=300)
        self._log(method="GET", url=url.split("?")[0], status=r.status_code, recipe=recipe, bytes=len(r.content))
        if r.status_code >= 400:
            raise HiggsfieldError(f"download {url.split('?')[0]} -> HTTP {r.status_code}")
        with open(dst, "wb") as f:
            f.write(r.content)
        return dst


def _summary(data):
    if not isinstance(data, dict):
        return data
    keep = {}
    for k in ("status", "request_id", "credits", "usd", "cost", "error", "detail", "message"):
        if k in data:
            keep[k] = data[k]
    if "images" in data:
        keep["images"] = len(data["images"] or [])
    if "video" in data:
        keep["video"] = bool(data["video"])
    if "_text" in data:
        keep["_text"] = data["_text"]
    return keep


def usd_of(estimate):
    """Best-effort USD from an estimate response."""
    if not isinstance(estimate, dict):
        return None
    for k in ("usd", "price_usd", "cost_usd", "cost"):
        v = estimate.get(k)
        if isinstance(v, (int, float)):
            return float(v)
        if isinstance(v, str):
            try:
                return float(v.strip("$"))
            except ValueError:
                pass
    return None


def stable_key(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(json.dumps(p, sort_keys=True).encode("utf-8"))
    return h.hexdigest()[:24]


def new_idempotency_key(cache_key):
    # Deterministic per recipe content: a crashed run that resubmits does not pay twice.
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "deadground-vfxgen/" + cache_key))
