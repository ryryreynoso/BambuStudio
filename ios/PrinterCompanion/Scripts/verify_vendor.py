#!/usr/bin/env python3
import pathlib,json,hashlib
root=pathlib.Path(__file__).resolve().parents[1]/'Vendor'
for manifest,base in [('engine-sha256.json',root/'OrcaBridgeCore.xcframework'),('libraries-sha256.json',root)]:
 for name,digest in json.loads((root/manifest).read_text()).items():
  file=base/name
  if not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest()!=digest:
   raise SystemExit('Native dependency missing or checksum mismatch: '+name)
print('Native dependencies verified')
