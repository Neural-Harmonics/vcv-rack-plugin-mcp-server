
import json
import urllib.request
import sys

base = "http://127.0.0.1:2600"

def req(method, path, body=None):
    data = json.dumps(body).encode() if body else None
    h = {"Content-Type": "application/json"} if data else {}
    req = urllib.request.Request(base + path, data=data, headers=h, method=method)
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.loads(resp.read())

def get_modules():
    return req("GET", "/modules")["data"]

def delete_module(mid):
    return req("DELETE", f"/modules/{mid}")

def add_module(plugin, slug):
    res = req("POST", "/modules/add", {"plugin": plugin, "slug": slug})
    if res["status"] == "ok":
        return res["data"]["id"]
    return None

def connect(out_id, out_port, in_id, in_port):
    req("POST", "/cables", {
        "outputModuleId": out_id,
        "outputId": out_port,
        "inputModuleId": in_id,
        "inputId": in_port
    })

# 1. Find existing Audio Interface
modules = get_modules()
audio_id = None
for m in modules:
    if "AudioInterface" in m["slug"]:
        audio_id = m["id"]
        print(f"Found existing audio interface: {m['slug']} (ID: {audio_id})")
        break

# 2. If not found, add one (but user says it won't be configured)
if not audio_id:
    print("No audio interface found, adding AudioInterface2...")
    audio_id = add_module("Core", "AudioInterface2")

# 3. Clean up other modules to start fresh
for m in modules:
    if m["id"] != audio_id and m["slug"] != "RackMcpServer":
        print(f"Deleting module {m['slug']} (ID: {m['id']})")
        try:
            delete_module(m["id"])
        except Exception as e:
            print(f"Could not delete {m['slug']}: {e}")

# 4. Add VCO, VCF, VCA
vco_id = add_module("Fundamental", "VCO")
vcf_id = add_module("Fundamental", "VCF")
vca_id = add_module("Fundamental", "VCA")

if not all([vco_id, vcf_id, vca_id, audio_id]):
    print("Failed to add all modules.")
    sys.exit(1)

# 5. Connect
# VCO SAW (out 2) -> VCF IN (in 0)
connect(vco_id, 2, vcf_id, 0)
# VCF LPF (out 0) -> VCA IN (in 0)
connect(vcf_id, 0, vca_id, 0)
# VCA OUT (out 0) -> Audio L/R (in 0 and 1)
connect(vca_id, 0, audio_id, 0)
connect(vca_id, 0, audio_id, 1)

# 6. Set VCF Freq to 0.5, VCA level to 1.0
req("POST", f"/modules/{vcf_id}/params", {"params": [{"id": 0, "value": 0.5}]})
req("POST", f"/modules/{vca_id}/params", {"params": [{"id": 0, "value": 1.0}]})

print("Patch rebuilt using existing audio interface!")
