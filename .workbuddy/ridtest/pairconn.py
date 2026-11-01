# -*- coding: utf-8 -*-
"""用 PC 的 BLE 去连设备，逼出配对流程（设备端会生成 6 位配对码并显示到屏幕）"""
import asyncio, sys
from bleak import BleakScanner, BleakClient

NAME_HINT = sys.argv[1] if len(sys.argv) > 1 else "RID"

async def main():
    print("scanning...", flush=True)
    devs = await BleakScanner.discover(10.0)
    target = None
    for d in devs:
        print("  found:", d.name, d.address, flush=True)
        if d.name and NAME_HINT.lower() in d.name.lower():
            target = d
    if not target:
        print("NOT FOUND")
        return
    print("connecting to", target.name, target.address, flush=True)
    try:
        async with BleakClient(target.address, timeout=25.0) as c:
            print("connected! services:", flush=True)
            for s in c.services:
                print("   ", s.uuid, [ch.uuid for ch in s.characteristics], flush=True)
            await asyncio.sleep(30)
    except Exception as e:
        print("connect error:", type(e).__name__, e, flush=True)

asyncio.run(main())
