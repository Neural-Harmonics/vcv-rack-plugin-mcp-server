#!/usr/bin/env python3
"""
Integration tests for the VCV Rack MCP HTTP Server.

Requires VCV Rack to be running with the MCP Server module enabled.
Run:
    python tests/test_server.py [--port PORT]

Each test connects to the live server and verifies the response shape and
semantics. Tests are ordered from least destructive (read-only) to more
invasive (add/remove modules, cables). The patch state is restored after
each invasive test.
"""

import sys
import json
import argparse
import urllib.request
import urllib.error

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"
SKIP = "\033[33mSKIP\033[0m"

results = {"pass": 0, "fail": 0, "skip": 0}


def request(method, url, body=None, timeout=5):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def check(name, condition, detail=""):
    if condition:
        print(f"  [{PASS}] {name}")
        results["pass"] += 1
    else:
        print(f"  [{FAIL}] {name}" + (f" — {detail}" if detail else ""))
        results["fail"] += 1


def run_test(name, fn):
    print(f"\n{name}")
    try:
        fn()
    except urllib.error.URLError as e:
        print(f"  [{FAIL}] Cannot reach server: {e.reason}")
        results["fail"] += 1
    except Exception as e:
        print(f"  [{FAIL}] Unexpected error: {e}")
        results["fail"] += 1


# ─── Individual test functions ──────────────────────────────────────────────

def test_status(base):
    resp = request("GET", f"{base}/status")
    check("response ok=true",      resp.get("status") == "ok")
    data = resp.get("data", {})
    check("data.server present",   "server" in data)
    check("data.version present",  "version" in data)
    check("data.sampleRate > 0",   data.get("sampleRate", 0) > 0,
          f"got {data.get('sampleRate')}")
    check("data.moduleCount >= 0", data.get("moduleCount", -1) >= 0)


def test_sample_rate(base):
    resp = request("GET", f"{base}/sample-rate")
    check("response ok=true",     resp.get("status") == "ok")
    sr = resp.get("data", {}).get("sampleRate", 0)
    check("sampleRate is numeric", isinstance(sr, (int, float)))
    check("sampleRate > 0",        sr > 0, f"got {sr}")


def test_library_all(base):
    resp = request("GET", f"{base}/library")
    check("response ok=true",      resp.get("status") == "ok")
    data = resp.get("data", [])
    check("data is a list",        isinstance(data, list))
    check("at least one plugin",   len(data) > 0, f"got {len(data)} plugins")
    if data:
        plug = data[0]
        check("plugin has slug",   "slug" in plug)
        check("plugin has modules","modules" in plug)
        check("modules is a list", isinstance(plug["modules"], list))


def test_library_fundamental(base):
    resp = request("GET", f"{base}/library/Fundamental")
    if resp.get("status") != "ok":
        print(f"  [{SKIP}] Fundamental plugin not installed — skipping")
        results["skip"] += 1
        return
    data = resp.get("data", {})
    check("slug == Fundamental",   data.get("slug") == "Fundamental")
    mods = data.get("modules", [])
    check("has modules",           len(mods) > 0)
    slugs = [m["slug"] for m in mods]
    for expected in ["VCO", "VCF", "VCA", "LFO"]:
        check(f"module {expected} present", expected in slugs,
              f"available: {slugs[:10]}...")


def test_library_not_found(base):
    try:
        request("GET", f"{base}/library/__nonexistent__plugin__")
        check("404 for unknown plugin", False, "expected HTTP 404 but got 200")
    except urllib.error.HTTPError as e:
        check("404 for unknown plugin", e.code == 404, f"got {e.code}")


def test_modules_list(base):
    resp = request("GET", f"{base}/modules")
    check("response ok=true",  resp.get("status") == "ok")
    data = resp.get("data", [])
    check("data is a list",    isinstance(data, list))
    if data:
        mod = data[0]
        check("module has id",   "id" in mod)
        check("module has slug", "slug" in mod)


def test_module_not_found(base):
    try:
        request("GET", f"{base}/modules/999999999")
        check("404 for unknown module", False, "expected HTTP 404")
    except urllib.error.HTTPError as e:
        check("404 for unknown module", e.code == 404, f"got {e.code}")


def test_add_inspect_remove(base):
    # Find a safe module to add — VCO from Fundamental plugin
    try:
        lib = request("GET", f"{base}/library/Fundamental")
    except urllib.error.HTTPError:
        print(f"  [{SKIP}] Fundamental plugin not available — skipping add/remove test")
        results["skip"] += 1
        return

    # Add VCO
    add_resp = request("POST", f"{base}/modules/add", {"plugin": "Fundamental", "slug": "VCO"})
    check("add returns ok",       add_resp.get("status") == "ok",
          json.dumps(add_resp))
    mod_id = add_resp.get("data", {}).get("id")
    check("add returns id",       mod_id is not None)
    if mod_id is None:
        return

    # Inspect the new module
    detail = request("GET", f"{base}/modules/{mod_id}")
    check("module detail ok",     detail.get("status") == "ok")
    data = detail.get("data", {})
    check("detail.id matches",    data.get("id") == mod_id)
    check("detail.slug == VCO",   data.get("slug") == "VCO")
    check("detail.width is numeric", isinstance(data.get("width"), (int, float)),
          f"got {data.get('width')!r}")
    check("detail.width > 0",     data.get("width", 0) > 0, f"got {data.get('width')}")
    check("detail has params",    len(data.get("params", [])) > 0)
    check("detail has outputs",   len(data.get("outputs", [])) > 0)

    # Params endpoint
    params_resp = request("GET", f"{base}/modules/{mod_id}/params")
    check("params ok",            params_resp.get("status") == "ok")
    params = params_resp.get("data", [])
    check("params is list",       isinstance(params, list))
    check("has at least 1 param", len(params) > 0)

    # Set a param (FREQ = 0.0 = A4)
    set_resp = request("POST", f"{base}/modules/{mod_id}/params",
                       {"params": [{"id": 0, "value": 0.0}]})
    check("set-param ok",         set_resp.get("status") == "ok")
    check("applied == 1",         set_resp.get("data", {}).get("applied") == 1)

    # Remove the module we added (clean up)
    del_resp = request("DELETE", f"{base}/modules/{mod_id}")
    check("remove ok",            del_resp.get("status") == "ok")

    # Verify it's gone
    try:
        request("GET", f"{base}/modules/{mod_id}")
        check("module gone after delete", False, "still accessible after delete")
    except urllib.error.HTTPError as e:
        check("module gone after delete", e.code == 404)


def test_cables_list(base):
    resp = request("GET", f"{base}/cables")
    check("response ok=true", resp.get("status") == "ok")
    data = resp.get("data", [])
    check("data is a list",   isinstance(data, list))
    if data:
        cable = data[0]
        check("cable has id",            "id" in cable)
        check("cable has outputModuleId","outputModuleId" in cable)
        check("cable has inputModuleId", "inputModuleId" in cable)


def test_connect_disconnect(base):
    try:
        request("GET", f"{base}/library/Fundamental")
        request("GET", f"{base}/library/Core")
    except urllib.error.HTTPError:
        print(f"  [{SKIP}] Required plugins not available — skipping cable test")
        results["skip"] += 1
        return

    # Add two modules to connect
    vco = request("POST", f"{base}/modules/add", {"plugin": "Fundamental", "slug": "VCO"})
    audio = request("POST", f"{base}/modules/add", {"plugin": "Core", "slug": "AudioInterface2"})
    if vco.get("status") != "ok" or audio.get("status") != "ok":
        print(f"  [{SKIP}] Could not add modules for cable test")
        results["skip"] += 1
        return

    vco_id   = vco["data"]["id"]
    audio_id = audio["data"]["id"]

    # Connect VCO SIN (out 0) → Audio L (in 0)
    conn_resp = request("POST", f"{base}/cables", {
        "outputModuleId": vco_id,
        "outputId":       0,
        "inputModuleId":  audio_id,
        "inputId":        0,
    })
    check("connect ok",        conn_resp.get("status") == "ok",
          json.dumps(conn_resp))
    cable_id = conn_resp.get("data", {}).get("id")
    check("connect returns id", cable_id is not None)

    if cable_id is not None:
        # Verify cable appears in list
        cables = request("GET", f"{base}/cables")
        ids = [c["id"] for c in cables.get("data", [])]
        check("cable in list", cable_id in ids)

        # Disconnect
        disc = request("DELETE", f"{base}/cables/{cable_id}")
        check("disconnect ok", disc.get("status") == "ok")

    # Clean up modules
    request("DELETE", f"{base}/modules/{vco_id}")
    request("DELETE", f"{base}/modules/{audio_id}")


def test_cors_headers(base):
    req = urllib.request.Request(f"{base}/status", method="GET")
    with urllib.request.urlopen(req, timeout=5) as resp:
        headers = {k.lower(): v for k, v in resp.headers.items()}
    check("CORS allow-origin header",
          "access-control-allow-origin" in headers,
          str(headers))


# ─── Main ───────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="VCV Rack MCP Server integration tests")
    parser.add_argument("--port", type=int, default=2600)
    args = parser.parse_args()
    base = f"http://127.0.0.1:{args.port}"

    print(f"Testing VCV Rack MCP Server at {base}\n{'─'*50}")

    # Connectivity check first
    try:
        request("GET", f"{base}/status", timeout=3)
    except urllib.error.URLError:
        print(f"[{FAIL}] Cannot connect to {base}")
        print("Make sure VCV Rack is running with the MCP Server module ON.")
        sys.exit(1)

    run_test("GET /status",          lambda: test_status(base))
    run_test("GET /sample-rate",     lambda: test_sample_rate(base))
    run_test("GET /library",         lambda: test_library_all(base))
    run_test("GET /library/Fundamental", lambda: test_library_fundamental(base))
    run_test("GET /library/unknown", lambda: test_library_not_found(base))
    run_test("GET /modules",         lambda: test_modules_list(base))
    run_test("GET /modules/unknown", lambda: test_module_not_found(base))
    run_test("GET /cables",          lambda: test_cables_list(base))
    run_test("CORS headers",         lambda: test_cors_headers(base))
    run_test("add / inspect / set-param / remove module",
             lambda: test_add_inspect_remove(base))
    run_test("connect / disconnect cable",
             lambda: test_connect_disconnect(base))

    print(f"\n{'─'*50}")
    print(f"Results: {results['pass']} passed, "
          f"{results['fail']} failed, "
          f"{results['skip']} skipped")

    sys.exit(0 if results["fail"] == 0 else 1)


if __name__ == "__main__":
    main()
