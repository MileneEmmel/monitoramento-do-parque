"""Non-destructive diagnostics for the fixed Freenove; never prints access keys."""
import argparse
import json
import time
import hashlib
from pathlib import Path
from urllib.request import Request, ProxyHandler, build_opener
from urllib.error import HTTPError
import serial
from serial.tools import list_ports

def discover(port='COM5', watch=0, reset=False):
    found = next((p for p in list_ports.comports() if p.device.upper() == port.upper()), None)
    if not found or (found.vid, found.pid, found.serial_number) != (6790, 21971, '5C93054388'):
        raise RuntimeError('Freenove verificada não encontrada; nenhuma porta foi aberta.')
    device = serial.Serial()
    device.port=port;device.baudrate=115200;device.timeout=.3;device.dtr=False;device.rts=False
    device.open()
    url=None; diagnostics=[]
    with device:
        if reset:
            device.rts=True;time.sleep(.15);device.rts=False
        deadline=time.monotonic()+max(75 if not watch else watch,15)
        next_query=0
        while time.monotonic()<deadline:
            if time.monotonic()>=next_query:
                device.write(b'@WIFI_STATUS\n');next_query=time.monotonic()+3
            line=device.readline().decode('utf-8','replace')
            if '@WIFI_STATUS ' in line:
                try:
                    status=json.loads(line.split('@WIFI_STATUS ',1)[1])
                    if status.get('connected'):url=status.get('url')
                except ValueError:pass
            elif any(s in line for s in ('PARK_SD','FREENOVE','Camera + AI OK','Guru Meditation','Stack canary','CORRUPT HEAP','assert failed','Brownout','camera','boot:','rst:','Backtrace:','WIFI_PREVIEW','wifi:','esp_video')):
                diagnostics.append(line.strip())
            if url and not watch:break
    if not url:raise RuntimeError('A ESP não confirmou o Wi-Fi pelo USB. Diagnósticos filtrados: '+repr(diagnostics))
    return url,diagnostics

def verify(url, sync_time=True):
    base,key=url.split('/#',1)
    opener=build_opener(ProxyHandler({}))
    def call(path,body=None,auth=True):
        headers={'Authorization':'Bearer '+key} if auth else {}
        data=None if body is None else json.dumps(body).encode()
        if data is not None:headers['Content-Type']='application/json'
        with opener.open(Request(base+path,data=data,headers=headers),timeout=20) as response:
            return response.read(), response.headers
    if sync_time:call('/api/time',{'epoch':int(time.time())})
    state=json.loads(call('/api/state')[0])
    assets=[]
    for path,mime in (('/','text/html'),('/app.js','text/javascript'),('/style.css','text/css'),('/folhagem-v2.webp','image/webp'),('/source-sans-3-v1.woff2','font/woff2')):
        started=time.monotonic()
        content,headers=call(path,auth=False)
        expected=Path(__file__).parent/'public'/('index.html' if path=='/' else path[1:])
        assert headers.get_content_type()==mime,(path,headers.get_content_type())
        assert hashlib.sha256(content).digest()==hashlib.sha256(expected.read_bytes()).digest(),path+' differs from local build'
        assets.append({'path':path,'bytes':len(content),'response_ms':round((time.monotonic()-started)*1000),'hash':'PASS'})
    try:call('/api/state',auth=False)
    except HTTPError as e:assert e.code==401
    else:raise AssertionError('API sem autenticação')
    for path,body in (('/api/wifi',{'ssid':'invalid','password':'x'}),('/api/reset',{'confirm':'NÃO'}),('/api/calibration',{'first':.9,'second':.1,'horizontal':False,'reverse':False})):
        try:call(path,body)
        except HTTPError as e:assert e.code==400,(path,e.code)
        else:raise AssertionError('Entrada inválida aceita: '+path)
    frames=[]
    for _ in range(3):
        t=time.monotonic();f=json.loads(call('/api/frame')[0]);frames.append({'received':f['received'],'age':f['age'],'response_ms':round((time.monotonic()-t)*1000),'image':bool(f['frame'] and f['frame'].get('jpeg'))});time.sleep(.7)
    assert all(f['image'] and f['age']<6 for f in frames),frames
    assert frames[-1]['received']>frames[0]['received'],frames
    csv,_=call('/api/export.csv?start='+state['today']+'&end='+state['today'])
    assert csv.startswith(b'data,entradas,saidas')
    return {'address':base,'today':state['today'],'totals':{k:state['device']['counter'][k] for k in ('entries','exits')},'storage':state['storage'],'report':{k:state['report'][k] for k in ('sample_count','entries','exits')},'assets':assets,'frames':frames,'csv_bytes':len(csv),'auth_and_invalid_inputs':'PASS'}

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--watch',type=int,default=0);parser.add_argument('--reboot',action='store_true');args=parser.parse_args()
    url,logs=discover(watch=args.watch,reset=args.reboot)
    persistence=None
    if args.reboot:
        before=verify(url)
        url,boot_logs=discover(watch=35,reset=True)
        logs.extend(boot_logs)
        after=verify(url,sync_time=False)
        assert after['storage']['ok'] and after['report']['sample_count']>=before['report']['sample_count']
        persistence={'before_samples':before['report']['sample_count'],'after_samples':after['report']['sample_count'],'before_totals':before['totals'],'after_totals':after['totals'],'result':'PASS'}
    print(json.dumps({'diagnostics':logs,'verification':verify(url),'reboot_persistence':persistence},ensure_ascii=True,indent=2))
