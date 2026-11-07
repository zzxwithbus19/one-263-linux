#!/usr/bin/env python3
"""离线 mock CDN：模拟 Mojang 的 version manifest / version json / libraries /
asset index / assets objects / Java runtime manifest，供 make check 使用。

所有返回内容都带真实 SHA1，能完整验证下载器的校验、缓存与并发路径。
用法: python3 tests/mock_cdn.py --port 18777
"""
import argparse
import hashlib
import io
import json
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

META = "https://piston-meta.mojang.com"
DATA = "https://piston-data.mojang.com"
LIBS = "https://libraries.minecraft.net"
ASSETS = "https://resources.download.minecraft.net"


def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def build_world():
    # client.jar 必须是真实 ZIP，构建器会打开它剥离 META-INF 签名
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("net/minecraft/client/main/Main.class", b"MOCK-CLASS-26.3")
        z.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
        z.writestr("META-INF/MOCK.SF", "Signature-Version: 1.0\n")
    client = bio.getvalue()
    lib = b"MOCK-LIBRARY-JAR-1.0" * 4
    asset = b"MOCK-ASSET-OBJECT" * 5
    java = b"#!/bin/sh\necho mock java 25\n"
    cs, ls, as_, js = sha1(client), sha1(lib), sha1(asset), sha1(java)

    lib_rel = "mock/lib/1.0/lib-1.0.jar"
    version = {
        "id": "26.3", "type": "release",
        "mainClass": "net.minecraft.client.main.Main",
        "javaVersion": {"component": "java-runtime-epsilon", "majorVersion": 25},
        "assetIndex": {"id": "34", "url": f"{META}/v1/packages/mock/34.json",
                       "sha1": "", "size": 0, "totalSize": len(asset)},
        "downloads": {"client": {"url": f"{DATA}/v1/objects/{cs}/client.jar",
                                  "sha1": cs, "size": len(client)}},
        "libraries": [{
            "name": "mock:lib:1.0",
            "downloads": {"artifact": {"path": lib_rel, "url": f"{LIBS}/{lib_rel}",
                                        "sha1": ls, "size": len(lib)}}
        }],
        "arguments": {"jvm": [], "game": []},
    }
    index = {"objects": {"minecraft/sounds/mock.ogg": {"hash": as_, "size": len(asset)}}}
    runtime_files = {
        "files": {
            "bin": {"type": "directory"},
            "bin/java": {"type": "file", "executable": True,
                         "downloads": {"raw": {"url": f"{DATA}/v1/objects/{js}/java",
                                                "sha1": js, "size": len(java)}}},
            "lib/modules": {"type": "file", "executable": False,
                            "downloads": {"raw": {"url": f"{DATA}/v1/objects/{js}/modules",
                                                   "sha1": js, "size": len(java)}}},
        }
    }
    version_bytes = json.dumps(version, indent=2).encode()
    index_bytes = json.dumps(index).encode()
    runtime_bytes = json.dumps(runtime_files).encode()

    version["assetIndex"]["sha1"] = sha1(index_bytes)
    version["assetIndex"]["size"] = len(index_bytes)
    version_bytes = json.dumps(version, indent=2).encode()
    vurl = f"{META}/v1/packages/mock/26.3.json"
    manifest = {
        "latest": {"release": "26.3", "snapshot": "26.4-snapshot-1"},
        "versions": [{"id": "26.3", "type": "release", "url": vurl,
                      "sha1": sha1(version_bytes), "time": "2026-11-01T00:00:00+00:00"}],
    }
    runtime_all = {
        "linux": {"java-runtime-epsilon": [{
            "availability": {"group": 1, "progress": 100},
            "manifest": {"url": f"{META}/v1/packages/mock/runtime.json",
                         "sha1": sha1(runtime_bytes), "size": len(runtime_bytes)},
            "version": {"name": "25.0.1", "released": "2025-12-10T00:00:00+00:00"},
        }]}
    }

    routes = {
        "/mc/game/version_manifest_v2.json": json.dumps(manifest).encode(),
        "/v1/packages/mock/26.3.json": version_bytes,
        "/v1/packages/mock/34.json": index_bytes,
        "/v1/packages/mock/runtime.json": runtime_bytes,
        f"/v1/objects/{cs}/client.jar": client,
        f"/{lib_rel}": lib,
        f"/{as_[:2]}/{as_}": asset,
        f"/v1/objects/{js}/java": java,
        f"/v1/objects/{js}/modules": java,
        "/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json":
            json.dumps(runtime_all).encode(),
    }
    return routes, {"client": cs, "lib": ls, "asset": as_, "java": js}


class Handler(BaseHTTPRequestHandler):
    routes = {}

    def do_GET(self):
        path = self.path.split("?")[0]
        data = self.routes.get(path)
        if data is None:
            self.send_error(404, "mock 404")
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18777)
    args = ap.parse_args()
    Handler.routes, hashes = build_world()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"mock cdn listening on 127.0.0.1:{args.port} assets_sha1={hashes['asset']}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
