"""Isolated output lifecycle reproduction with the game's genuine Streamline DLLs."""
import argparse
import ctypes
from ctypes import wintypes
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

p=argparse.ArgumentParser()
for name in ('directory','exe','addon','reshade','sdk-fg','sdk-ll','streamline'):
    p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--isolate-nvidia-hooks',action='store_true')
p.add_argument('--stock-streamline',action='store_true')
p.add_argument('--generate',action='store_true')
p.add_argument('--latency-output',choices=('XeLL','Reflex'),default='XeLL')
p.add_argument('--early-addon',action='store_true')
a=p.parse_args()
d=a.directory.resolve()/datetime.datetime.now().strftime('real-streamline-%Y%m%d-%H%M%S-%f')
d.mkdir(parents=True)
inputs={a.exe:a.exe.name,a.reshade:'dxgi.dll',
        a.sdk_fg:'libxess_fg.dll',a.sdk_ll:'libxell.dll'}
if not a.stock_streamline:
    inputs[a.addon]='renodx-mfgunlock-native.addon64'
for name in ('sl.interposer.dll','sl.common.dll','sl.reflex.dll','sl.pcl.dll','sl.dlss_g.dll','nvngx_dlssg.dll'):
    inputs[a.streamline/name]=name
manifest={}
for source,name in inputs.items():
    shutil.copy2(source,d/name)
    manifest[name]=hashlib.sha256((d/name).read_bytes()).hexdigest()
(d/'MFGAdaUnlock.ini').write_text('[Backend]\nSelected=XeMFG\n[XeFG]\nMultiplier=4\nForceBorderless=0\n[Latency]\nOutput='+a.latency_output+'\n')
(d/'ReShade.ini').write_text('[GENERAL]\nNoDebugInfo=1\n[OVERLAY]\nTutorialProgress=4\n')
ctypes.windll.kernel32.SetErrorMode(3)
process=subprocess.Popen([str(d/a.exe.name),'4','--real-streamline',*(['--isolate-nvidia-hooks'] if a.isolate_nvidia_hooks else []),*(['--stock-streamline'] if a.stock_streamline else []),*(['--generate'] if a.generate else []),*(['--early-addon'] if a.early_addon else [])],cwd=d,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,creationflags=0x08000000)
timed_out=False
try:
    stdout,stderr=process.communicate(timeout=60)
except subprocess.TimeoutExpired:
    timed_out=True
    # Only our child process is inspected; no game process is attached.
    import msvcrt
    dump=ctypes.WinDLL('Dbghelp.dll',use_last_error=True).MiniDumpWriteDump
    dump.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.HANDLE,wintypes.DWORD,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p]
    dump.restype=wintypes.BOOL
    with (d/'isolated-timeout.dmp').open('wb') as output:
        dump(wintypes.HANDLE(int(process._handle)),process.pid,wintypes.HANDLE(msvcrt.get_osfhandle(output.fileno())),0x1000,None,None,None)
    process.kill()
    stdout,stderr=process.communicate(timeout=10)
events=[json.loads(line) for line in (d/'MFGAdaUnlock-XeMFG.log').read_text().splitlines()] if (d/'MFGAdaUnlock-XeMFG.log').exists() else []
isolated=any(e.get('event')=='native_presentation_isolation' and e.get('isolated') for e in events)
shutdown=any(e.get('event')=='native_streamline_shutdown' and e.get('phase')=='complete' and not e.get('result') for e in events)
inference='Real Streamline inference cycle 3 tensor launches=' in stdout
(d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=process.returncode,timed_out=timed_out,stdout=stdout,stderr=stderr,
    presentation_isolated=isolated,shutdown_completed=shutdown,inference_verified=inference,latency_output=a.latency_output,early_addon=a.early_addon),indent=2))
print(stdout,end='');print(stderr,end='');print('Evidence:',d)
raise SystemExit(0 if not timed_out and process.returncode==0 and 'PASS:' in stdout and (a.stock_streamline or (isolated and shutdown)) and (not a.generate or inference) else 1)
