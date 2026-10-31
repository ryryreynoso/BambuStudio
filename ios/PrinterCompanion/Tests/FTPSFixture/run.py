import pathlib,ssl,socket,subprocess,threading,tempfile
base=pathlib.Path(__file__).resolve().parent
root=base.parents[1]
# Only the fixed port differs in this localhost fixture. Production always uses 990.
source=(root/'NativeFTPS/PrinterFTPS.cpp').read_text().replace(':990',':19990')
with tempfile.TemporaryDirectory(prefix='ftps-compile-') as build:
 implementation=pathlib.Path(build)/'PrinterFTPS.cpp'
 implementation.write_text(source)
 subprocess.run(['clang++','-std=c++17',str(base/'main.cpp'),str(implementation),'-I'+str(root/'NativeFTPS'),'-lcurl','-o',str(pathlib.Path(build)/'fixture')],check=True)
 fixture=pathlib.Path(build)/'fixture'
 # Keep the built executable available after the compilation scope.
 executable=base/'fixture'
 import shutil
 shutil.copy2(fixture,executable)
with tempfile.TemporaryDirectory(prefix='phone-ftps-') as directory:
 p=pathlib.Path(directory)
 def openssl(*args):subprocess.run(['openssl',*args],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 openssl('req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(p/'ca.key'),'-out',str(p/'ca.crt'),'-days','1','-subj','/CN=Phone FTPS fixture CA')
 openssl('req','-new','-newkey','rsa:2048','-nodes','-keyout',str(p/'server.key'),'-out',str(p/'server.csr'),'-subj','/CN=TESTSERIAL123')
 (p/'ext').write_text('subjectAltName=DNS:TESTSERIAL123\nextendedKeyUsage=serverAuth\n')
 openssl('x509','-req','-in',str(p/'server.csr'),'-CA',str(p/'ca.crt'),'-CAkey',str(p/'ca.key'),'-CAcreateserial','-out',str(p/'server.crt'),'-days','1','-extfile',str(p/'ext'))
 openssl('req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(p/'unrelated.key'),'-out',str(p/'unrelated.crt'),'-days','1','-subj','/CN=Unrelated CA')
 payload=b'fixture bytes\x00\xff\n' * 1024
 (p/'file.3mf').write_bytes(payload)
 for mode in ['success','reject-upload','wrong-identity','wrong-ca']:
  listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);listener.bind(('127.0.0.1',19990));listener.listen(1);listener.settimeout(10)
  ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(p/'server.crt',p/'server.key')
  seen=[];uploaded=[];errors=[]
  def serve():
   data_listener=None
   try:
    raw,_=listener.accept()
    with ctx.wrap_socket(raw,server_side=True) as s:
     s.settimeout(10);s.sendall(b'220 fixture FTPS\r\n');f=s.makefile('rb')
     while True:
      line=f.readline()
      if not line:break
      command=line.decode().strip().split(' ',1)[0].upper();seen.append(command)
      if command=='USER':s.sendall(b'331 password required\r\n')
      elif command=='PASS':s.sendall(b'230 authenticated\r\n')
      elif command=='PWD':s.sendall(b'257 "/"\r\n')
      elif command in ['PBSZ','PROT','TYPE','CWD']:s.sendall(b'200 OK\r\n')
      elif command=='EPSV':
       data_listener=socket.socket();data_listener.bind(('127.0.0.1',0));data_listener.listen(1);data_listener.settimeout(10)
       port=data_listener.getsockname()[1];s.sendall(f'229 Entering Extended Passive Mode (|||{port}|)\r\n'.encode())
      elif command=='STOR':
       if mode=='reject-upload':s.sendall(b'550 storage failure\r\n');return
       s.sendall(b'150 transfer\r\n');draw,_=data_listener.accept()
       data=b''
       with ctx.wrap_socket(draw,server_side=True) as d:
        d.settimeout(10)
        while True:
         chunk=d.recv(65536)
         if not chunk:break
         data+=chunk
       uploaded.append(data);s.sendall(b'226 complete\r\n')
      elif command=='QUIT':s.sendall(b'221 bye\r\n');break
      else:s.sendall(b'200 OK\r\n')
   except ssl.SSLError:
    if mode not in ['wrong-identity','wrong-ca']:errors.append('TLS failure')
   except Exception as error:errors.append(str(error))
   finally:
    if data_listener:data_listener.close()
    listener.close()
  worker=threading.Thread(target=serve);worker.start()
  serial='OTHERPRINTER' if mode=='wrong-identity' else 'TESTSERIAL123'
  ca=p/'unrelated.crt' if mode=='wrong-ca' else p/'ca.crt'
  result=subprocess.run([str(base/'fixture'),str(p/'file.3mf'),serial,str(ca),'unused'],capture_output=True,text=True,timeout=20)
  worker.join(12)
  assert not errors,(mode,errors,result.stdout)
  assert (result.returncode==0)==(mode=='success'),(mode,result.stdout)
  if mode=='success':assert uploaded==[payload] and 'PROT' in seen
  if mode in ['wrong-identity','wrong-ca']:assert 'PASS' not in seen and not uploaded
  print(mode,'PASS',flush=True)
