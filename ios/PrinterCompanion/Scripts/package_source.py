#!/usr/bin/env python3
"""Package this exact iOS app and its complete pinned dependency sources. Never packages app data."""
import argparse, hashlib, pathlib, tarfile, json, io
p=argparse.ArgumentParser()
p.add_argument('--engine-source',type=pathlib.Path,required=True)
p.add_argument('--openssl-source',type=pathlib.Path,required=True)
p.add_argument('--curl-source',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
archives=[(a.engine_source,'ff0bc921fc756f17fbdea064c5d1ce678e56c3a3183474562b5d12bb0d83f591'),(a.openssl_source,'603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a'),(a.curl_source,'e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49')]
for file,expected in archives:
 digest=hashlib.sha256(file.read_bytes()).hexdigest()
 if expected and digest!=expected:raise SystemExit('Source archive checksum mismatch: '+file.name)
 print(file.name,digest,flush=True)
a.output.parent.mkdir(parents=True,exist_ok=True)
manifest={}
with tarfile.open(a.output,'w:gz',compresslevel=6) as archive:
 for file in sorted(root.rglob('*')):
  rel=file.relative_to(root)
  if not file.is_file() or file.is_symlink():continue
  if any(part in ['xcuserdata','.git','build','DerivedData','__pycache__'] or part.endswith('.xcodeproj') for part in rel.parts):continue
  if 'OrcaBridgeCoreSimulator' in rel.parts:continue
  if file.name in ['CorrespondingSource.tar.gz','fixture','.DS_Store'] or file.resolve()==a.output.resolve():continue
  manifest["ios/PrinterCompanion/"+str(rel)]=hashlib.sha256(file.read_bytes()).hexdigest()
  archive.add(file,arcname='ios/PrinterCompanion/'+str(rel),recursive=False)
 for file in sorted((root.parents[1]/'shared/AppleCamera').glob('*.swift')):
  rel='shared/AppleCamera/'+file.name
  manifest[rel]=hashlib.sha256(file.read_bytes()).hexdigest()
  archive.add(file,arcname=rel,recursive=False)
 for file in sorted((root.parents[1]/'tools/home-relay').glob('*')):
  if not file.is_file() or file.suffix not in ['.py','.md','.txt','.swift']:continue
  rel='tools/home-relay/'+file.name
  manifest[rel]=hashlib.sha256(file.read_bytes()).hexdigest()
  archive.add(file,arcname=rel,recursive=False)
 data=json.dumps(manifest,sort_keys=True,indent=2).encode()
 info=tarfile.TarInfo('SourceManifest.json');info.size=len(data)
 archive.addfile(info,io.BytesIO(data))
 for file,_ in archives:archive.add(file,arcname='Dependencies/'+file.name,recursive=False)
print('Wrote',a.output,flush=True)
