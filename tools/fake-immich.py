#!/usr/bin/env python3
"""A small stand-in for an Immich server, for tests: the parts of the API
Photos uses (login, the sync stream with acknowledgements, thumbnails,
originals, upload check, upload, favourites, trash), kept in memory.

  tools/fake-immich.py [--port 2283] [--seed DIR]

  user: test@example.com  password: secret  (or the API key "test-key")

Test hooks (no sign-in needed):
  POST /admin/reset                   forget everything
  POST /admin/seed?dir=PATH           add every picture under PATH as an asset
  POST /admin/delete?id=ASSET         the server deletes an asset for good
  POST /admin/trash?id=ASSET          ... moves it to its trash
  POST /admin/favorite?id=ASSET&on=1
  POST /admin/sync-reset              the next sync stream starts with SyncResetV1
  GET  /admin/assets                  what the server has (JSON)
"""
import argparse
import base64
import hashlib
import io
import json
import os
import re
import threading
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

LOCK = threading.Lock()
STATE = {}


def reset():
    STATE.clear()
    STATE.update(assets={}, log=[], sessions={}, seq=0, force_reset=False)


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")


def log(kind, data):
    """Appends a change; the sync stream replays what a session has not acked."""
    STATE["seq"] += 1
    STATE["log"].append((STATE["seq"], kind, data))


def thumbnail(data, edge):
    try:
        from PIL import Image
        img = Image.open(io.BytesIO(data))
        img = img.convert("RGB")
        img.thumbnail((edge, edge))
        out = io.BytesIO()
        img.save(out, "JPEG", quality=80)
        return out.getvalue(), img.size
    except Exception:
        return None, (0, 0)


def add_asset(name, data, created=None, favorite=False):
    checksum = base64.b64encode(hashlib.sha1(data).digest()).decode()
    for a in STATE["assets"].values():
        if a["checksum"] == checksum and not a["deleted"]:
            return a, True
    width = height = 0
    taken = created or now()
    try:
        from PIL import Image
        img = Image.open(io.BytesIO(data))
        width, height = img.size
        exif = img.getexif()
        orientation = exif.get(0x0112, 1)
        if orientation >= 5:
            width, height = height, width
        when = exif.get_ifd(0x8769).get(0x9003) or exif.get(0x0132)
        if when:
            taken = datetime.strptime(when, "%Y:%m:%d %H:%M:%S").strftime("%Y-%m-%dT%H:%M:%S.000Z")
    except Exception:
        pass
    asset = dict(id=str(uuid.uuid4()), name=name, data=data, checksum=checksum,
                 taken=taken, uploaded=now(), favorite=favorite, trashed=None,
                 deleted=False, width=width, height=height,
                 video=name.lower().endswith((".mov", ".mp4")))
    STATE["assets"][asset["id"]] = asset
    log("asset", asset["id"])
    return asset, False


def asset_event(a):
    return {"type": "AssetV2", "data": {
        "id": a["id"], "ownerId": "user-1", "originalFileName": a["name"],
        "fileCreatedAt": a["taken"], "fileModifiedAt": a["taken"], "createdAt": a["uploaded"],
        "localDateTime": a["taken"], "type": "VIDEO" if a["video"] else "IMAGE",
        "deletedAt": a["trashed"], "isFavorite": a["favorite"], "visibility": "timeline",
        "duration": None, "livePhotoVideoId": None, "stackId": None, "libraryId": None,
        "width": a["width"] or None, "height": a["height"] or None, "isEdited": False,
        "checksum": a["checksum"], "thumbhash": None}}


def exif_event(a):
    return {"type": "AssetExifV1", "data": {
        "assetId": a["id"], "description": "", "exifImageWidth": a["width"] or None,
        "exifImageHeight": a["height"] or None, "fileSizeInByte": len(a["data"]),
        "orientation": "1", "dateTimeOriginal": a["taken"], "modifyDate": a["taken"],
        "timeZone": None, "latitude": None, "longitude": None, "projectionType": None,
        "city": None, "state": None, "country": None, "make": "Fake", "model": "Server 1",
        "lensModel": None, "fNumber": 2.0, "focalLength": 4.2, "iso": 100,
        "exposureTime": "1/100", "profileDescription": None, "rating": None, "fps": None}}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", content_type="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def session(self):
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer ") and auth[7:] in STATE["sessions"]:
            return STATE["sessions"][auth[7:]]
        if self.headers.get("x-api-key") == "test-key":
            return STATE["sessions"].setdefault("api-key", {"ack": 0})
        return None

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", "0") or 0))

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_PUT(self):
        self.route("PUT")

    def do_DELETE(self):
        self.route("DELETE")

    def route(self, method):
        url = urlparse(self.path)
        path, query = url.path, parse_qs(url.query)
        raw = self.body()
        with LOCK:
            try:
                self.dispatch(method, path, query, raw)
            except BrokenPipeError:
                pass

    def dispatch(self, method, path, query, raw):
        if path.startswith("/admin/"):
            return self.admin(method, path, query)
        if path == "/api/auth/login" and method == "POST":
            data = json.loads(raw or b"{}")
            if data.get("email") != "test@example.com" or data.get("password") != "secret":
                return self.reply(401, {"message": "Incorrect email or password"})
            token = uuid.uuid4().hex
            STATE["sessions"][token] = {"ack": 0}
            return self.reply(201, {"accessToken": token, "userId": "user-1",
                                    "userEmail": "test@example.com", "name": "Test User",
                                    "isAdmin": False, "isOnboarded": True,
                                    "profileImagePath": "", "shouldChangePassword": False})
        if path == "/api/server/ping":
            return self.reply(200, {"res": "pong"})
        session = self.session()
        if session is None:
            return self.reply(401, {"message": "Authentication required"})
        if path == "/api/auth/logout":
            return self.reply(200, {"successful": True})
        if path == "/api/users/me":
            return self.reply(200, {"id": "user-1", "email": "test@example.com", "name": "Test User"})
        if path == "/api/server/version":
            return self.reply(200, {"major": 3, "minor": 4, "patch": 1})
        if path == "/api/sync/stream" and method == "POST":
            return self.sync_stream(session, json.loads(raw or b"{}"))
        if path == "/api/sync/ack" and method == "POST":
            for ack in json.loads(raw or b"{}").get("acks", []):
                m = re.match(r"^\w+\|(\d+)$", ack)
                if m:
                    session["ack"] = max(session["ack"], int(m.group(1)))
            return self.reply(204)
        m = re.match(r"^/api/assets/([0-9a-f-]+)/(thumbnail|original)$", path)
        if m and method == "GET":
            a = STATE["assets"].get(m.group(1))
            if a is None or a["deleted"]:
                return self.reply(404, {"message": "Not found"})
            if m.group(2) == "original":
                return self.reply(200, a["data"], "application/octet-stream")
            edge = 1440 if query.get("size", ["thumbnail"])[0] == "preview" else 250
            data, _ = thumbnail(a["data"], edge)
            if data is None:
                return self.reply(404, {"message": "No thumbnail"})
            return self.reply(200, data, "image/jpeg")
        if path == "/api/assets/bulk-upload-check" and method == "POST":
            results = []
            for item in json.loads(raw or b"{}").get("assets", []):
                found = next((a for a in STATE["assets"].values()
                              if a["checksum"] == item["checksum"] and not a["deleted"]), None)
                if found:
                    results.append({"id": item["id"], "action": "reject", "reason": "duplicate",
                                    "assetId": found["id"], "isTrashed": found["trashed"] is not None})
                else:
                    results.append({"id": item["id"], "action": "accept"})
            return self.reply(200, {"results": results})
        if path == "/api/assets" and method == "POST":
            return self.upload(raw)
        if path == "/api/assets" and method == "PUT":
            data = json.loads(raw or b"{}")
            for asset_id in data.get("ids", []):
                a = STATE["assets"].get(asset_id)
                if a and "isFavorite" in data:
                    a["favorite"] = bool(data["isFavorite"])
                    log("asset", a["id"])
            return self.reply(204)
        if path == "/api/assets" and method == "DELETE":
            data = json.loads(raw or b"{}")
            for asset_id in data.get("ids", []):
                a = STATE["assets"].get(asset_id)
                if not a:
                    continue
                if data.get("force"):
                    a["deleted"] = True
                    log("delete", a["id"])
                else:
                    a["trashed"] = now()
                    log("asset", a["id"])
            return self.reply(204)
        return self.reply(404, {"message": "Not found: " + path})

    def sync_stream(self, session, request):
        lines = []
        reset_wanted = request.get("reset") or STATE["force_reset"] and not request.get("reset")
        if STATE["force_reset"] and not request.get("reset"):
            STATE["force_reset"] = False
            lines.append({"type": "SyncResetV1", "data": {}, "ack": "SyncResetV1|0"})
        else:
            start = 0 if request.get("reset") else session["ack"]
            if request.get("reset"):
                session["ack"] = 0
            assets, exifs = [], []
            for seq, kind, asset_id in STATE["log"]:
                if seq <= start:
                    continue
                a = STATE["assets"][asset_id]
                if kind == "delete":
                    # A reset lists what exists; deletions are what is missing.
                    if not request.get("reset"):
                        assets.append({"type": "AssetDeleteV1", "data": {"assetId": asset_id},
                                       "ack": "AssetDeleteV1|%d" % seq})
                elif not a["deleted"]:
                    event = asset_event(a)
                    event["ack"] = "AssetV2|%d" % seq
                    assets.append(event)
                    event = exif_event(a)
                    event["ack"] = "AssetExifV1|%d" % seq
                    exifs.append(event)
            lines += assets + exifs
            lines.append({"type": "SyncCompleteV1", "data": {},
                          "ack": "SyncCompleteV1|%d" % STATE["seq"]})
        body = "".join(json.dumps(line) + "\n" for line in lines).encode()
        self.reply(200, body, "application/jsonlines+json")

    def upload(self, raw):
        content_type = self.headers.get("Content-Type", "")
        m = re.search(r"boundary=(.+)", content_type)
        if not m:
            return self.reply(400, {"message": "multipart expected"})
        boundary = m.group(1).strip('"').encode()
        fields, data, name = {}, None, None
        for part in raw.split(b"--" + boundary):
            if b"\r\n\r\n" not in part:
                continue
            head, content = part.split(b"\r\n\r\n", 1)
            content = content[:-2] if content.endswith(b"\r\n") else content
            head = head.decode("utf-8", "replace")
            field = re.search(r'name="([^"]*)"', head)
            filename = re.search(r'filename="([^"]*)"', head)
            if not field:
                continue
            if field.group(1) == "assetData":
                data = content
                name = filename.group(1) if filename else "upload"
            else:
                fields[field.group(1)] = content.decode("utf-8", "replace")
        if data is None or "fileCreatedAt" not in fields or "fileModifiedAt" not in fields:
            return self.reply(400, {"message": "assetData, fileCreatedAt and fileModifiedAt are required"})
        for unknown in set(fields) - {"fileCreatedAt", "fileModifiedAt", "filename", "isFavorite",
                                      "duration", "visibility", "livePhotoVideoId"}:
            return self.reply(400, {"message": "property %s should not exist" % unknown})
        checksum = self.headers.get("x-immich-checksum")
        actual = base64.b64encode(hashlib.sha1(data).digest()).decode()
        if checksum and checksum != actual and checksum.lower() != hashlib.sha1(data).hexdigest():
            return self.reply(400, {"message": "checksum mismatch"})
        asset, duplicate = add_asset(fields.get("filename") or name, data,
                                     fields["fileCreatedAt"], fields.get("isFavorite") == "true")
        self.reply(200 if duplicate else 201,
                   {"id": asset["id"], "status": "duplicate" if duplicate else "created"})

    def admin(self, method, path, query):
        asset_id = query.get("id", [""])[0]
        if path == "/admin/reset":
            reset()
            return self.reply(200, {"ok": True})
        if path == "/admin/seed":
            count = 0
            for root, _, names in os.walk(query.get("dir", ["."])[0]):
                for name in sorted(names):
                    if name.lower().endswith((".jpg", ".jpeg", ".png", ".webp", ".tif", ".raf", ".dng", ".heic")):
                        with open(os.path.join(root, name), "rb") as f:
                            _, duplicate = add_asset(name, f.read())
                        count += 0 if duplicate else 1
            return self.reply(200, {"added": count})
        if path == "/admin/assets":
            return self.reply(200, [{k: v for k, v in a.items() if k != "data"}
                                    for a in STATE["assets"].values() if not a["deleted"]])
        if path == "/admin/sync-reset":
            STATE["force_reset"] = True
            return self.reply(200, {"ok": True})
        a = STATE["assets"].get(asset_id)
        if a is None:
            return self.reply(404, {"message": "no such asset"})
        if path == "/admin/delete":
            a["deleted"] = True
            log("delete", a["id"])
        elif path == "/admin/trash":
            a["trashed"] = now()
            log("asset", a["id"])
        elif path == "/admin/favorite":
            a["favorite"] = query.get("on", ["1"])[0] == "1"
            log("asset", a["id"])
        else:
            return self.reply(404, {"message": "Not found"})
        self.reply(200, {"ok": True})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2283)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--seed")
    args = parser.parse_args()
    reset()
    if args.seed:
        for root, _, names in os.walk(args.seed):
            for name in sorted(names):
                if name.lower().endswith((".jpg", ".jpeg", ".png", ".webp", ".tif")):
                    with open(os.path.join(root, name), "rb") as f:
                        add_asset(name, f.read())
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print("fake Immich on port %d, %d assets" % (args.port, len(STATE["assets"])), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
