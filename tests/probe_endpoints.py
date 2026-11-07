#!/usr/bin/env python3
"""诊断 mc263-fetch 使用的下载端点（只读，不写文件）。

用法: python3 tests/probe_endpoints.py [--version 26.3]
"""
import argparse
import json
import sys
import urllib.request

UA = {"User-Agent": "mc263-fetch/1.0"}


def get(url, timeout=12, limit=8 * 1024 * 1024):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read(limit)
        return r.status, data


def as_json(data):
    try:
        return json.loads(data)
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="26.3")
    args = ap.parse_args()

    manifest_url = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
    try:
        code, data = get(manifest_url)
        m = as_json(data)
        print(f"[manifest] HTTP {code} latest={m['latest']} versions={len(m['versions'])}")
        hit = [v for v in m["versions"] if v["id"] == args.version]
        if not hit:
            print(f"[manifest] 找不到 {args.version}，最近的 26.x:")
            print([v["id"] for v in m["versions"] if v["id"].startswith("26.")][:10])
            return 1
        vurl = hit[0]["url"]
        print(f"[manifest] {args.version} json url={vurl}")
    except Exception as e:
        print(f"[manifest] 失败: {e}")
        return 1

    try:
        code, data = get(vurl)
        v = as_json(data)
        print(f"[version] HTTP {code} id={v.get('id')} type={v.get('type')} "
              f"assetIndex={v.get('assetIndex', {}).get('id')} javaVersion={v.get('javaVersion')}")
        print(f"[version] mainClass={v.get('mainClass')} libraries={len(v.get('libraries', []))}")
        print(f"[version] client={json.dumps(v.get('downloads', {}).get('client'))[:240]}")
        print(f"[version] lib[0]={json.dumps(v.get('libraries', [{}])[0])[:520]}")
        print(f"[version] assetIndex.url={v.get('assetIndex', {}).get('url')}")
        idx_url = v.get("assetIndex", {}).get("url")
    except Exception as e:
        print(f"[version] 失败: {e}")
        return 1

    if idx_url:
        try:
            code, data = get(idx_url, limit=32 * 1024 * 1024)
            idx = as_json(data)
            objs = idx.get("objects", {}) if idx else {}
            print(f"[assets] HTTP {code} objects={len(objs)} "
                  f"total_size={sum(o.get('size', 0) for o in objs.values())}")
            first = next(iter(objs.items()), None)
            if first:
                h = first[1]["hash"]
                print(f"[assets] 示例对象 URL=https://resources.download.minecraft.net/{h[:2]}/{h}")
        except Exception as e:
            print(f"[assets] 失败: {e}")

    # Java runtime manifest（多个候选，取第一个可用的）
    runtime_urls = [
        "https://launchermeta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json",
        "https://piston-meta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json",
    ]
    for u in runtime_urls:
        try:
            code, data = get(u, limit=32 * 1024 * 1024)
            j = as_json(data)
            print(f"[runtime] HTTP {code} url={u}")
            print(f"[runtime] top keys={list(j.keys())[:8]}")
            # 结构: {"linux-x64": {"java-runtime-epsilon": [{"manifest": {...}}]}} 之类
            def walk(node, path="", depth=0):
                if depth > 3 or not isinstance(node, dict):
                    return
                for k, val in node.items():
                    p = f"{path}/{k}"
                    if depth >= 1 and ("linux" in k or "java-runtime" in k):
                        print(f"[runtime]   {p} -> {type(val).__name__} "
                              f"{list(val.keys())[:6] if isinstance(val, dict) else len(val) if isinstance(val, list) else ''}")
                    walk(val, p, depth + 1)
            walk(j)
            try:
                entry = j["linux"]["java-runtime-epsilon"][0]
                print(f"[runtime] epsilon entry keys={list(entry.keys())} version={entry.get('version')}")
                mu = entry["manifest"]["url"]
                print(f"[runtime] epsilon manifest url={mu}")
                c2, d2 = get(mu, limit=32 * 1024 * 1024)
                rm = as_json(d2)
                files = rm.get("files", {}) if rm else {}
                print(f"[runtime] manifest HTTP {c2} files={len(files)}")
                for k in list(files)[:3]:
                    print(f"[runtime]   {k} -> {json.dumps(files[k])[:300]}")
                print(f"[runtime] manifest keys={list(rm.keys())[:10] if rm else []}")
            except Exception as e:
                print(f"[runtime] 细节解析失败: {e}")
            break
        except Exception as e:
            print(f"[runtime] {u} 失败: {e}")

    try:
        code, data = get("https://meta.fabricmc.net/v2/versions/loader?limit=1", limit=64 * 1024)
        j = as_json(data)
        print(f"[fabric] HTTP {code} latest loader={j[0].get('version') if j else None}")
    except Exception as e:
        print(f"[fabric] 失败: {e}")

    for name, u in [
        ("bmclapi", "https://bmclapi2.bangbang93.com/mc/game/version_manifest_v2.json"),
        ("libraries", "https://libraries.minecraft.net/"),
    ]:
        try:
            code, data = get(u, timeout=8, limit=1024)
            print(f"[mirror] {name} HTTP {code} bytes={len(data)}")
        except Exception as e:
            print(f"[mirror] {name} 失败: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
