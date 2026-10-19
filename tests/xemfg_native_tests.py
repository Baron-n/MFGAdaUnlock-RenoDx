"""Run the native ReShade transport in a separate directory, never in a game."""
import argparse
import ctypes
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ap=argparse.ArgumentParser()
for name in ('directory','addon','exe','fixture','reshade','sdk-fg','sdk-ll'):
    ap.add_argument('--'+name,type=Path,required=True)
ap.add_argument('--multiplier',type=int,choices=range(2,7),default=4)
args=ap.parse_args()
d=args.directory.resolve()/datetime.datetime.now().strftime('native-%Y%m%d-%H%M%S-%f')
d.mkdir(parents=True)
inputs={args.addon:'renodx-mfgunlock-native.addon64',args.exe:args.exe.name,
        args.fixture:'sl.interposer.dll',args.reshade:'dxgi.dll',
        args.sdk_fg:'libxess_fg.dll',args.sdk_ll:'libxell.dll'}
manifest={}
for src,name in inputs.items():
    shutil.copy2(src,d/name)
    manifest[name]=hashlib.sha256((d/name).read_bytes()).hexdigest()
(d/'MFGAdaUnlock.ini').write_text(f'[Backend]\nSelected=XeMFG\n[XeFG]\nMultiplier={args.multiplier}\n',encoding='ascii')
(d/'ReShade.ini').write_text('[GENERAL]\nNoDebugInfo=1\n[OVERLAY]\nTutorialProgress=4\n',encoding='ascii')
ctypes.windll.kernel32.SetErrorMode(3)
result=subprocess.run([str(d/args.exe.name),str(args.multiplier)],cwd=d,capture_output=True,text=True,timeout=120,creationflags=0x08000000)
(d/'result.json').write_text(json.dumps({'inputs':manifest,'exit':result.returncode,
    'stdout':result.stdout,'stderr':result.stderr},indent=2),encoding='utf-8')
print(result.stdout,end=''); print(result.stderr,end=''); print('Evidence:',d)
raise SystemExit(0 if result.returncode==0 and 'PASS:' in result.stdout else 1)
