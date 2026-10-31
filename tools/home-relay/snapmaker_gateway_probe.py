"""Read-only diagnostics for an already-running official Snapmaker connection gateway.
Does not launch it, log in, export tokens, bind printers or issue commands.
"""
import argparse,json,urllib.request
class NoRedirect(urllib.request.HTTPRedirectHandler):
 def redirect_request(self,*args,**kwargs):return None

def inspect(port):
 if not 1<=port<=65535:raise ValueError('Invalid local gateway port')
 opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect())
 def get(path):
  with opener.open(f'http://127.0.0.1:{port}'+path,timeout=5) as response:
   body=response.read(2*1024*1024+1)
   if len(body)>2*1024*1024:raise ValueError('Oversized gateway response')
   return json.loads(body)
 health=get('/api/health')
 ready=(health.get('status')=='ok' or health.get('ok') is True) and all(health.get('components',{}).get(k)=='ok' for k in ['ipc_server','web_server'])
 account=get('/api/account') if ready else {}
 data=account.get('data',account)
 logged_in=data.get('is_login',data.get('isLogin',False)) is True
 # Do not echo the account frame: official versions may include the access token.
 return {'gateway_ready':ready,'signed_in':logged_in,'device_connected':health.get('device_connected') is True,'cloud_print_verified':False,'cloud_camera_verified':False}
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--port',type=int,required=True);a=p.parse_args()
 try:print(json.dumps(inspect(a.port)))
 except Exception:raise SystemExit('Official gateway unavailable or incompatible. Open Snapmaker Orca and check its connection component.')
