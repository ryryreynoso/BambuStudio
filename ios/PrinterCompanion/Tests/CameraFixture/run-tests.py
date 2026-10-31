#!/usr/bin/env python3
"""Build and run local TLS camera fixtures on macOS. No printer access or stored credentials."""
import os,pathlib,shutil,subprocess,tempfile
source=pathlib.Path(__file__).resolve().parent
repo=source.parents[3]
with tempfile.TemporaryDirectory(prefix='camera-fixture-') as directory:
 root=pathlib.Path(directory)
 def run(*args):subprocess.run(args,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 for name in ['ca','wrong-ca']:
  run('openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(root/(name+'.key')),'-out',str(root/(name+'.pem')),'-days','2','-subj','/CN='+name)
  run('openssl','x509','-in',str(root/(name+'.pem')),'-outform','DER','-out',str(root/(name+'.der')))
 run('openssl','req','-newkey','rsa:2048','-nodes','-keyout',str(root/'leaf.key'),'-out',str(root/'leaf.csr'),'-subj','/CN=CAMERA123')
 run('openssl','x509','-req','-in',str(root/'leaf.csr'),'-CA',str(root/'ca.pem'),'-CAkey',str(root/'ca.key'),'-CAcreateserial','-out',str(root/'leaf.pem'),'-days','2')
 core=repo/'shared/AppleCamera'
 subprocess.run(['xcrun','swiftc','-module-cache-path',str(root/'module-cache'),*[str(core/name) for name in ['CameraProtocol.swift','CameraStream.swift','CameraTrust.swift','BambuCertificates.swift']],str(source/'main.swift'),'-o',str(root/'probe')],check=True)
 shutil.copy2(source/'video.json',root/'video.json')
 subprocess.run(['python3','-u',str(source/'run.py')],env={**os.environ,'CAMERA_FIXTURE_DIR':str(root)},check=True)
