import sys, time, urllib.request, json

HOST = "192.168.4.1"

def get(path, timeout=8):
    try:
        with urllib.request.urlopen("http://" + HOST + path, timeout=timeout) as r:
            return r.read().decode()
    except Exception as e:
        return "ERR %s %s" % (type(e).__name__, str(e)[:60])

def post(path, obj, timeout=10):
    data = json.dumps(obj).encode()
    req = urllib.request.Request("http://" + HOST + path, data=data,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode()
    except Exception as e:
        return "ERR %s %s" % (type(e).__name__, str(e)[:60])

def dump(path=".workbuddy/ridtest/dumps"):
    import os
    os.makedirs(path, exist_ok=True)
    return path

modes = ["rid", "camera", "relay"]
for m in modes:
    print("==== switch to %s ====" % m)
    print(post("/api/mode", {"mode": m}))
    time.sleep(16)
    print("mode now:", get("/api/mode")[:120])
    # force UI pages and dump
    for ui, name in [(1, "func"), (2, "detail"), (3, "gap"), (4, "confirm")]:
        r = post("/api/oled", {"menu": ui})
        time.sleep(1.2)
        d = get("/api/oled_dump")
        fn = "%s_%s_%d.txt" % (m, name, ui)
        p = dump()
        open(p + "/" + fn, "w").write(d if d.startswith("ERR") else d)
        # crude sanity: count non-empty rows
        rows = [l for l in d.split("\n") if l.strip("#. ")]
        print("  menu=%d -> ui_json=%s  dump_rows=%d" % (ui, r[:60], len(rows)))
    print()
print("done")
