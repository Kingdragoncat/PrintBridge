"""
PrintBridge OTA Firmware Flasher
Usage:
    python ota_flash.py [ip_or_hostname]

Example:
    python ota_flash.py 192.168.18.52
    python ota_flash.py mini
    python ota_flash.py printbridge.local
"""

import os
import sys
import time
import requests

target = sys.argv[1] if len(sys.argv) > 1 else os.getenv("PRINTBRIDGE_HOST", "192.168.18.52")
url = f"http://{target}:9101/update"

bin_path = os.path.join("software", ".pio", "build", "esp32s3", "firmware.bin")
if not os.path.exists(bin_path):
    print(f"Error: {bin_path} not found! Run 'pio run' first.")
    sys.exit(1)

size = os.path.getsize(bin_path)
print(f"Found firmware.bin: {size} bytes ({size / 1024:.1f} KB)")
print(f"Uploading to {url} via OTA...")

start = time.time()
try:
    with open(bin_path, "rb") as f:
        files = {"update": ("firmware.bin", f, "application/octet-stream")}
        res = requests.post(url, files=files, timeout=90)
        print(f"Response ({res.status_code}): {res.text.strip()}")
        if res.status_code == 200 and "OK" in res.text:
            print(f"OTA Upload successful in {time.time() - start:.1f}s! Board is restarting...")
        else:
            print("OTA Upload returned non-OK status.")
except Exception as e:
    print(f"OTA Upload error: {e}")
