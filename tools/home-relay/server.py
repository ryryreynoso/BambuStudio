"""Loopback-only U1 relay; publish ONLY behind an authenticated HTTPS tunnel.
No manufacturer credentials or arbitrary URL/command forwarding. Pairing token in Keychain.
"""
import asyncio, hmac, json, pathlib, re, sqlite3, subprocess, tempfile
from aiohttp import web, ClientSession, ClientTimeout, ClientError, FormData, WSMsgType

SAFE_FILE = re.compile(r'^[A-Za-z0-9_-]+\.gcode$')
OBJECTS = {'print_stats','display_status','heater_bed','extruder','extruder1','extruder2','extruder3','exception_manager'}

def make_app(upstream, token, database, *, read_only=False):
    if len(token) < 32: raise ValueError('Use a random pairing token of at least 32 characters')
    upload_lock = asyncio.Lock()
    db=sqlite3.connect(database)
    db.execute('CREATE TABLE IF NOT EXISTS uploads(path TEXT PRIMARY KEY, dispatched INTEGER NOT NULL DEFAULT 0)'); db.commit()
    @web.middleware
    async def authenticate(request, handler):
        if not hmac.compare_digest(request.headers.get('Authorization','').encode('utf-8'), ('Bearer '+token).encode('utf-8')):
            raise web.HTTPUnauthorized()
        if read_only and (request.method != 'GET' or request.path.endswith('/websocket')):
            raise web.HTTPForbidden(text='Relay is in read-only verification mode')
        try: return await handler(request)
        except (asyncio.TimeoutError, OSError, ClientError): raise web.HTTPBadGateway(text='Home printer unavailable')
    app=web.Application(middlewares=[authenticate],client_max_size=256*1024*1024)
    async def lifecycle(app):
        app['http']=ClientSession(timeout=ClientTimeout(total=120,connect=5))
        yield
        await app['http'].close(); db.close()
    app.cleanup_ctx.append(lifecycle)
    async def read(request):
        path=request.match_info['path']
        if path=='printer/objects/query':
            if not set(request.query).issubset(OBJECTS): raise web.HTTPForbidden()
        elif path!='server/files/camera/monitor.jpg': raise web.HTTPNotFound()
        async with app['http'].get(upstream+'/'+path,params=request.query,allow_redirects=False) as response:
            if response.status!=200: raise web.HTTPBadGateway(text='Printer status or camera unavailable')
            data=bytearray()
            async for chunk in response.content.iter_chunked(65536):
                data.extend(chunk)
                if len(data)>4*1024*1024: raise web.HTTPBadGateway()
            return web.Response(body=data,content_type='image/jpeg' if path.endswith('.jpg') else 'application/json',headers={'Cache-Control':'no-store'})
    async def upload(request):
        # Serialize uploads so duplicate paths cannot race past the durable check.
        async with upload_lock:
            return await upload_locked(request)
    async def upload_locked(request):
        if not request.content_type.startswith('multipart/'):
            raise web.HTTPBadRequest(text='Expected multipart upload')
        reader=await request.multipart(); filename=None; count=0
        with tempfile.TemporaryFile() as file:
            async for part in reader:
                if part.name=='file':
                    if filename is not None or not SAFE_FILE.fullmatch(part.filename or ''): raise web.HTTPBadRequest()
                    filename=part.filename
                    while True:
                        chunk=await part.read_chunk()
                        if not chunk: break
                        count+=len(chunk)
                        if count>256*1024*1024: raise web.HTTPRequestEntityTooLarge(max_size=256*1024*1024,actual_size=count)
                        file.write(chunk)
                else:
                    value=await part.read_chunk(8192)
                    if not part.at_eof(): raise web.HTTPBadRequest()
                    if part.name not in {'root','path','print'}: raise web.HTTPBadRequest()
                    if part.name=='print' and value!=b'false': raise web.HTTPBadRequest(text='Upload cannot start a print')
            if not filename or not count: raise web.HTTPBadRequest()
            if db.execute('SELECT 1 FROM uploads WHERE path=?',(filename,)).fetchone(): raise web.HTTPConflict()
            file.seek(0); form=FormData()
            form.add_field('root','gcodes');form.add_field('path','');form.add_field('print','false')
            form.add_field('file',file,filename=filename,content_type='application/octet-stream')
            async with app['http'].post(upstream+'/server/files/upload',data=form,allow_redirects=False) as response:
                if response.status not in (200,201): raise web.HTTPBadGateway(text='Printer upload failed')
                raw=await response.content.read(65537)
                if len(raw)>65536: raise web.HTTPBadGateway(text='Invalid upload response')
                try: result=json.loads(raw)
                except (ValueError, UnicodeError): raise web.HTTPBadGateway(text='Invalid upload response')
                if not isinstance(result,dict) or not isinstance(result.get('item'),dict): raise web.HTTPBadGateway(text='Invalid upload response')
                if result.get('item',{}).get('path')!=filename or result['item'].get('root')!='gcodes': raise web.HTTPBadGateway(text='Unconfirmed printer upload')
                db.execute('INSERT INTO uploads(path) VALUES(?)',(filename,)); db.commit()
                return web.json_response(result)
    async def socket(request):
        ws=web.WebSocketResponse(max_msg_size=65536,heartbeat=20);await ws.prepare(request)
        async for message in ws:
            # URLSessionWebSocketTask.send(.data) sends binary JSON frames on iPhone.
            if message.type not in (WSMsgType.TEXT, WSMsgType.BINARY): break
            frame = None
            try:
                frame=json.loads(message.data)
                if not isinstance(frame,dict) or frame.get('jsonrpc')!='2.0' or not isinstance(frame.get('id'),(str,int)): raise ValueError()
                params=frame.get('params',{})
                if not isinstance(params,dict): raise ValueError()
                path=params.get('path','')
                if not isinstance(path,str): raise ValueError()
                if frame.get('method')!='server.files.start_local_print' or not SAFE_FILE.fullmatch(path): raise ValueError()
                if params.get('type')!='gcode' or params.get('print_plate')!=1: raise ValueError()
                if set(params)-{'type','path','print_plate','options','extra_info'}: raise ValueError()
                options=params.get('options',{})
                if not isinstance(options,dict) or set(options)-{'bed_level','flow_calibrate','time_lapse_camera','map_table'}: raise ValueError()
                if any(options.get(key,0) not in (0,1) for key in ('bed_level','flow_calibrate','time_lapse_camera')): raise ValueError()
                mapping=options.get('map_table',[[0,0]])
                if not isinstance(mapping,list) or len(mapping)!=1 or not isinstance(mapping[0],list) or len(mapping[0])!=2 or mapping[0][0]!=0 or mapping[0][1] not in (0,1,2,3): raise ValueError()
                if params.get('extra_info',{'print_task_id':''}) != {'print_task_id':''}: raise ValueError()
                # Durable claim BEFORE transmission: unknown outcomes are never automatically resent.
                cursor=db.execute('UPDATE uploads SET dispatched=1 WHERE path=? AND dispatched=0',(path,));db.commit()
                if cursor.rowcount!=1: raise ValueError()
                async with app['http'].ws_connect(upstream.replace('http','ws',1)+'/websocket',max_msg_size=1024*1024,timeout=10) as target:
                    await target.send_json(frame)
                    # One deadline, even if the printer sends unrelated telemetry continuously.
                    async with asyncio.timeout(30):
                        while True:
                            reply=await target.receive_json()
                            if isinstance(reply,dict) and reply.get('id')==frame.get('id'):
                                await ws.send_json(reply); break
            except Exception:
                await ws.send_json({'jsonrpc':'2.0','id':frame.get('id') if isinstance(frame,dict) else None,'error':{'code':-32001,'message':'Relay could not confirm the command. Check printer status before retrying.'}})
        return ws
    app.router.add_get('/v1/u1/websocket',socket)
    app.router.add_get('/v1/u1/{path:printer/objects/query|server/files/camera/monitor.jpg}',read)
    app.router.add_post('/v1/u1/server/files/upload',upload)
    return app

if __name__=='__main__':
    import argparse,urllib.parse,os
    p=argparse.ArgumentParser();p.add_argument('--printer',required=True);p.add_argument('--port',type=int,default=9841);p.add_argument('--state',type=pathlib.Path,required=True)
    p.add_argument('--read-only',action='store_true',help='Disable uploads and print commands for connection verification')
    a=p.parse_args();u=urllib.parse.urlsplit(a.printer)
    if u.scheme not in ('http','https') or not u.hostname or u.username or u.password or u.query or u.fragment or u.path not in ('','/'):p.error('Use a fixed home printer origin')
    a.state.mkdir(parents=True,exist_ok=True);os.chmod(a.state,0o700)
    token=subprocess.check_output(['security','find-generic-password','-s','3DPrint.HomeRelay','-a','owner','-w'],text=True).strip()
    web.run_app(make_app(a.printer.rstrip('/'),token,str(a.state/'relay.sqlite'),read_only=a.read_only),host='127.0.0.1',port=a.port,access_log=None,print=None)
