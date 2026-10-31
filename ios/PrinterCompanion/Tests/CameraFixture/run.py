#!/usr/bin/env python3
"""Local-only TLS/RTSP integration tests. Synthetic credentials; never talks to a printer."""
import ssl,socket,threading,subprocess,pathlib,hashlib,re,time,struct,json,base64,os
root=pathlib.Path(os.environ['CAMERA_FIXTURE_DIR'])
ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(root/'leaf.pem',root/'leaf.key')
video=json.loads((root/'video.json').read_text())
md5=lambda x:hashlib.md5(x.encode()).hexdigest()

def run(mode,serial='CAMERA123',seconds=5,ca='ca.der'):
 listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(5);listener.settimeout(.5)
 port=listener.getsockname()[1];stop=threading.Event();errors=[];connections=[]
 def serve():
  while not stop.is_set():
   try: raw,_=listener.accept()
   except socket.timeout:continue
   except OSError:return
   connections.append(1)
   try:
    with ctx.wrap_socket(raw,server_side=True) as c:
     c.settimeout(1);buf=b''
     while not stop.is_set():
      try:data=c.recv(65536)
      except socket.timeout:continue
      if not data:break
      buf+=data
      while buf:
       if buf[0]==36:
        if len(buf)<4:break
        n=int.from_bytes(buf[2:4],'big')
        if len(buf)<4+n:break
        buf=buf[4+n:];continue
       if b'\r\n\r\n' not in buf:break
       raw,buf=buf.split(b'\r\n\r\n',1);text=raw.decode();lines=text.split('\r\n');method,uri,_=lines[0].split(' ')
       fields=dict((k.lower(),v.strip()) for k,v in (x.split(':',1) for x in lines[1:]))
       extra='';body='';status=200
       if mode=='stall':continue
       if 'authorization' not in fields or mode=='reject':
        status=401;extra='WWW-Authenticate: Digest realm="Fixture", nonce="test-nonce"\r\n'
       else:
        auth=dict(re.findall(r'(\w+)="([^"]*)"',fields['authorization']))
        expected=md5(md5('bblp:Fixture:fixture-code')+':test-nonce:'+md5(method+':'+uri))
        assert auth.get('response')==expected,'Invalid digest response'
        if method=='DESCRIBE':
         if mode=='malformed':c.sendall(b'garbage\r\n\r\n');break
         body='v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;sprop-parameter-sets='+','.join(video['sets'])+'\r\na=control:track1\r\n'
         extra=f'Content-Base: rtsps://127.0.0.1:322/streaming/live/1/\r\n'
        if method=='SETUP':extra='Session: fixture;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n'
       response=f'RTSP/1.0 {status} OK\r\nCSeq: {fields["cseq"]}\r\n{extra}Content-Length: {len(body)}\r\n\r\n{body}'.encode()
       # Deliberately fragment the RTSP status line across TCP writes.
       c.sendall(response[:11]);c.sendall(response[11:])
       if method=='PLAY' and status==200:
        if mode=='disconnect':break
        for i,nal in enumerate(video['nals']):
         packet=bytes([128,224 if i==len(video['nals'])-1 else 96,0,i+1,0,0,0,1,0,0,0,1])+base64.b64decode(nal)
         c.sendall(b'$\0'+struct.pack('!H',len(packet))+packet)
      else:continue
      if mode in ['disconnect','malformed']:break
   except (ssl.SSLError,ConnectionError,OSError):pass
   except Exception as e:errors.append(str(e))
 thread=threading.Thread(target=serve,daemon=True);thread.start()
 p=subprocess.run([str(root/'probe'),str(root/ca),serial,str(port),str(seconds)],capture_output=True,text=True,timeout=seconds+8)
 stop.set();listener.close();thread.join(2)
 assert p.returncode==0,(mode,p.stderr)
 assert not errors,(mode,errors)
 return p.stdout,len(connections)

text,n=run('ok',seconds=7);assert 'DECODED 32x32' in text and 'Live' in text,text;print('PASS verified TLS, Digest, DESCRIBE/SETUP/PLAY and native H264 sample')
text,n=run('reject');assert 'rejected the LAN access code' in text and n==1,text;print('PASS rejected access code stops without reconnect loop')
text,n=run('ok',serial='WRONG123');assert 'certificate verification failed' in text and 'SAMPLE' not in text,text;print('PASS wrong printer identity rejected before authentication')
text,n=run('disconnect',seconds=8);assert n>=2 and 'Reconnecting' in text,text;print('PASS disconnected stream reconnects with new authenticated session')
text,n=run('malformed');assert 'Invalid camera response' in text and 'SAMPLE' not in text,text;print('PASS malformed stream rejected')
text,n=run('stall',seconds=13);assert 'timed out' in text,text;print('PASS silent server times out')

text,n=run('ok',ca='wrong-ca.der');assert 'certificate verification failed' in text and 'SAMPLE' not in text,text;print('PASS untrusted certificate authority rejected')
text,n=run('disconnect',seconds=1);assert n==1,text;print('PASS stop cancels pending reconnect')
