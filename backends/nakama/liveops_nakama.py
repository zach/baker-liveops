"""baker-liveops's Nakama backend, as a command (the publisher's "cmd:" backend protocol).

    liveops_nakama.py --url URL [--client] get ENV/KEY     the bytes on stdout; exit 3: none
    liveops_nakama.py --url URL put ENV/KEY                 the bytes on stdin

KEY is static.hpp's layout: "manifest" or "overlays/NAME-HASH.dta". Publishing calls the
server-to-server RPC with the runtime's http_key (NAKAMA_HTTP_KEY). With --client, reads go
the way a game's do: a device session (NAKAMA_SERVER_KEY), then the liveops_manifest and
liveops_overlay RPCs. Standard library only. Keys are read from the environment and never
printed.
"""

import argparse
import base64
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid


def call(url, rpc, payload, token=None, http_key=None):
    query = "?" + urllib.parse.urlencode({"http_key": http_key}) if http_key else ""
    req = urllib.request.Request(f"{url}/v2/rpc/{rpc}{query}", data=json.dumps(json.dumps(payload)).encode(),
                                 method="POST", headers={"Content-Type": "application/json"})
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(json.loads(r.read())["payload"])


def session(url, server_key):
    device = os.environ.get("LIVEOPS_DEVICE_ID") or "liveops-" + uuid.uuid4().hex
    req = urllib.request.Request(f"{url}/v2/account/authenticate/device?create=true",
                                 data=json.dumps({"id": device}).encode(), method="POST",
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Basic " + base64.b64encode(
                                              (server_key + ":").encode()).decode()})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())["token"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:7350")
    ap.add_argument("--client", action="store_true", help="read as a game client does")
    ap.add_argument("op", choices=["get", "put"])
    ap.add_argument("key")
    a = ap.parse_args()
    env, _, key = a.key.partition("/")
    http_key = os.environ.get("NAKAMA_HTTP_KEY", "defaulthttpkey")
    try:
        if a.op == "put":
            if a.client:
                sys.exit("a client does not publish")
            body = sys.stdin.buffer.read().decode("utf-8")
            call(a.url, "liveops_put", {"env": env, "key": key, "body": body}, http_key=http_key)
            return 0
        if a.client:
            token = session(a.url, os.environ.get("NAKAMA_SERVER_KEY", "defaultkey"))
            rpc = "liveops_manifest" if key == "manifest" else "liveops_overlay"
            reply = call(a.url, rpc, {"env": env, "key": key}, token=token)
        else:
            rpc = "liveops_manifest" if key == "manifest" else "liveops_overlay"
            reply = call(a.url, rpc, {"env": env, "key": key}, http_key=http_key)
        sys.stdout.buffer.write(reply["body"].encode("utf-8"))
        return 0
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return 3
        print(f"liveops_nakama: {a.op} {a.key}: HTTP {e.code}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, OSError) as e:
        print(f"liveops_nakama: {a.op} {a.key}: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
