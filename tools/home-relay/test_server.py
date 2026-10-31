import unittest, tempfile, importlib.util, pathlib, json
from aiohttp import web, FormData
from aiohttp.test_utils import TestServer, TestClient
spec=importlib.util.spec_from_file_location('relay', str(pathlib.Path(__file__).with_name('server.py')));relay=importlib.util.module_from_spec(spec);spec.loader.exec_module(relay)
class RelayTests(unittest.IsolatedAsyncioTestCase):
 async def asyncSetUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.starts=0
  async def status(r):return web.json_response({'result':{'status':{'print_stats':{'state':'standby'}}}})
  async def upload(r):
   self.fake_uploads+=1
   data=await r.post();self.assertEqual(data['print'],'false')
   return web.json_response({'item':{'path':data['file'].filename,'root':'gcodes'}})
  async def socket(r):
   ws=web.WebSocketResponse();await ws.prepare(r)
   async for msg in ws:
    import json
    frame=json.loads(msg.data);self.starts+=1;await ws.send_json({'jsonrpc':'2.0','id':frame['id'],'result':{}})
   return ws
  self.fake_uploads=0
  fake=web.Application();fake.router.add_get('/printer/objects/query',status);fake.router.add_post('/server/files/upload',upload);fake.router.add_get('/websocket',socket)
  self.printer=TestServer(fake);await self.printer.start_server()
  self.client=TestClient(TestServer(relay.make_app(str(self.printer.make_url('')).rstrip('/'),'x'*32,self.tmp.name+'/state.sqlite')));await self.client.start_server()
  self.headers={'Authorization':'Bearer '+'x'*32}
 async def asyncTearDown(self):await self.client.close();await self.printer.close();self.tmp.cleanup()
 async def test_auth_and_scoped_routes(self):
  self.assertEqual((await self.client.get('/v1/u1/printer/objects/query')).status,401)
  self.assertEqual((await self.client.get('/v1/u1/printer/objects/query?print_stats',headers=self.headers)).status,200)
  self.assertEqual((await self.client.get('/v1/u1/printer/objects/query?configfile',headers=self.headers)).status,403)
  self.assertEqual((await self.client.post('/printer/gcode/script',headers=self.headers)).status,404)
 async def test_upload_never_prints_and_duplicate_dispatch_blocked(self):
  form=FormData();form.add_field('print','false');form.add_field('file',b'G1 X1',filename='phone-fixture.gcode',content_type='application/octet-stream')
  self.assertEqual((await self.client.post('/v1/u1/server/files/upload',data=form,headers=self.headers)).status,200);self.assertEqual(self.starts,0)
  ws=await self.client.ws_connect('/v1/u1/websocket',headers=self.headers)
  frame={'jsonrpc':'2.0','id':1,'method':'server.files.start_local_print','params':{'path':'phone-fixture.gcode','type':'gcode','print_plate':1}}
  await ws.send_json(frame);self.assertIn('result',await ws.receive_json());self.assertEqual(self.starts,1)
  await ws.send_json(frame);self.assertIn('error',await ws.receive_json());self.assertEqual(self.starts,1)
  await ws.close()
 async def test_upload_cannot_implicitly_start(self):
  form=FormData();form.add_field('print','true');form.add_field('file',b'G1 X1',filename='phone-fixture.gcode')
  self.assertEqual((await self.client.post('/v1/u1/server/files/upload',data=form,headers=self.headers)).status,400);self.assertEqual(self.starts,0)
 async def test_iphone_binary_json_start(self):
  form=FormData();form.add_field('print','false');form.add_field('file',b'G1 X1',filename='phone-binary.gcode',content_type='application/octet-stream')
  self.assertEqual((await self.client.post('/v1/u1/server/files/upload',data=form,headers=self.headers)).status,200)
  ws=await self.client.ws_connect('/v1/u1/websocket',headers=self.headers)
  frame={'jsonrpc':'2.0','id':'iphone-id','method':'server.files.start_local_print','params':{'path':'phone-binary.gcode','type':'gcode','print_plate':1,'options':{'bed_level':1,'flow_calibrate':0,'time_lapse_camera':0,'map_table':[[0,3]]},'extra_info':{'print_task_id':''}}}
  await ws.send_bytes(json.dumps(frame).encode())
  reply=await ws.receive_json(timeout=2)
  self.assertEqual(reply['id'],'iphone-id');self.assertIn('result',reply);self.assertEqual(self.starts,1)
  await ws.close()
 async def test_malformed_socket_does_not_disconnect(self):
  ws=await self.client.ws_connect('/v1/u1/websocket',headers=self.headers)
  for value in ['[]','null','{','{"jsonrpc":"2.0","id":1,"params":null}']:
   await ws.send_str(value);self.assertIn('error',await ws.receive_json(timeout=2))
  self.assertEqual(self.starts,0);await ws.close()
 async def test_read_only_rejects_upload_and_socket(self):
  client=TestClient(TestServer(relay.make_app(str(self.printer.make_url('')).rstrip('/'),'x'*32,self.tmp.name+'/readonly.sqlite',read_only=True)))
  await client.start_server()
  try:
   self.assertEqual((await client.get('/v1/u1/printer/objects/query?print_stats',headers=self.headers)).status,200)
   self.assertEqual((await client.post('/v1/u1/server/files/upload',headers=self.headers)).status,403)
   self.assertEqual((await client.get('/v1/u1/websocket',headers=self.headers)).status,403)
   self.assertEqual(self.starts,0);self.assertEqual(self.fake_uploads,0)
  finally: await client.close()
 async def test_dispatch_claim_survives_relay_restart(self):
  await self.test_upload_never_prints_and_duplicate_dispatch_blocked()
  await self.client.close()
  self.client=TestClient(TestServer(relay.make_app(str(self.printer.make_url('')).rstrip('/'),'x'*32,self.tmp.name+'/state.sqlite')));await self.client.start_server()
  ws=await self.client.ws_connect('/v1/u1/websocket',headers=self.headers)
  await ws.send_json({'jsonrpc':'2.0','id':2,'method':'server.files.start_local_print','params':{'path':'phone-fixture.gcode','type':'gcode','print_plate':1}})
  self.assertIn('error',await ws.receive_json(timeout=2));self.assertEqual(self.starts,1);await ws.close()
if __name__=='__main__':unittest.main()
