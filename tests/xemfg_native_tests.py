"""Run the native ReShade transport in a separate directory, never in a game."""
import argparse
import ctypes
import configparser
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
ap.add_argument('--latency-output',choices=('XeLL','Reflex'),default='XeLL')
ap.add_argument('--runtime-layout',choices=('adjacent','storage-only','preloaded-storage','mutated-storage','missing'),default='adjacent')
ap.add_argument('--overlay-keys',action='store_true')
ap.add_argument('--overlay-foreground',action='store_true')
ap.add_argument('--capcom-flags',action='store_true')
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
if args.runtime_layout!='adjacent':
    storage=d/'_storage_'; storage.mkdir()
    for name in ('libxess_fg.dll','libxell.dll'):
        if args.runtime_layout!='missing':
            shutil.copy2(d/name,storage/name)
        if args.runtime_layout in ('storage-only','missing'):
            (d/name).unlink()
    if args.runtime_layout=='mutated-storage':
        # Change only the unused DOS stub, retaining a loadable PE image with
        # the original exports and instruction profiles but an unadmitted SHA.
        path=storage/'libxess_fg.dll'
        content=bytearray(path.read_bytes()); content[0x40]^=1; path.write_bytes(content)
        manifest['_storage_/libxess_fg.dll']=hashlib.sha256(content).hexdigest()
(d/'MFGAdaUnlock.ini').write_text(f'[Backend]\nSelected=XeMFG\n[XeFG]\nMultiplier={args.multiplier}\n[Latency]\nOutput={args.latency_output}\n',encoding='ascii')
(d/'ReShade.ini').write_text('[GENERAL]\nNoDebugInfo=1\n[INPUT]\nInputProcessing=2\nKeyOverlay=36,0,0,0\n[OVERLAY]\nTutorialProgress=4\n',encoding='ascii')
ctypes.windll.kernel32.SetErrorMode(3)
flags=(['--overlay-foreground'] if args.overlay_foreground else ['--overlay-keys'] if args.overlay_keys else [])
if args.capcom_flags: flags+=['--capcom-flags']
if args.runtime_layout in ('preloaded-storage','mutated-storage'): flags+=['--preload-runtimes']
if args.runtime_layout in ('mutated-storage','missing'): flags+=['--expect-startup-rejection']
result=subprocess.run([str(d/args.exe.name),str(args.multiplier),*flags],cwd=d,capture_output=True,text=True,timeout=120,creationflags=0x08000000)
prefs=configparser.ConfigParser(); prefs.read(d/'MFGAdaUnlock.ini',encoding='ascii')
events=[json.loads(line) for line in (d/'MFGAdaUnlock-XeMFG.log').read_text(encoding='utf-8').splitlines()] if (d/'MFGAdaUnlock-XeMFG.log').exists() else []
if args.runtime_layout in ('mutated-storage','missing'):
    expected='unreviewed_Intel_runtime_pair' if args.runtime_layout=='mutated-storage' else 'missing_Intel_runtime:libxess_fg.dll'
    refused=any(e.get('event')=='startup_rejected' and e.get('reason')==expected for e in events)
    unpublished=not any(e.get('event') in ('ready','runtime_selection','native_context_created') for e in events)
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,runtime_layout=args.runtime_layout,exit=result.returncode,
        stdout=result.stdout,stderr=result.stderr,startup_rejection_verified=refused,model_unpublished=unpublished),indent=2))
    print(result.stdout,end=''); print(result.stderr,end=''); print('Evidence:',d)
    raise SystemExit(0 if result.returncode==0 and 'PASS:' in result.stdout and refused and unpublished else 1)
selections=[e for e in events if e.get('event')=='runtime_selection']
runtime_selection=bool(selections) and all('_storage_' in e['provider_path'] and '_storage_' in e['xell_path'] for e in selections) if args.runtime_layout!='adjacent' else bool(selections)
sdk_deadlines=[e for e in events if e.get('event')=='dlssg_interval_deadline' and e.get('sdk_deadlines')]
policy=[e.get('deadlines') for e in events if e.get('event')=='pacing_policy']
policy_activation=policy==['sdk','adaptive']
# An occluded output may not execute the deadline body. Distinguish that lack
# of observation from either proving passthrough or witnessing a violation.
passthrough=all(e['deadline_ns']==e['sdk_deadline_ns'] and not e['flip_metered'] for e in sdk_deadlines) if sdk_deadlines else None
hud_filter=len([e for e in events if e.get('event')=='native_hud_filtered'])>=2
passthrough_identity=any(e.get('event')=='native_bypass_present_id' and e.get('id')==35 and e.get('result')==0 for e in events)
native_keys=(('Overlay key transitions: opens=2 closes=2' in result.stdout
    and 'Overlay keys after resize/FG-off: opens=3 closes=3' in result.stdout
    and 'Production panel draws=' in result.stdout) if args.overlay_keys else None)
native_child=(('Overlay keys with child input: opens=4 closes=4' in result.stdout) if args.overlay_keys else None)
foreground_verified=('Foreground ReShade: opens=6 closes=6; production panel drawn; game commands captured/released' in result.stdout) if args.overlay_foreground else None
persisted=(prefs.get('Latency','Input',fallback='')=='Reflex'
    and prefs.get('Latency','Output',fallback='')==args.latency_output
    and prefs.get('XeFG','ForceBorderless',fallback='')=='0'
    and prefs.get('XeFG','XeFGFlipMetering',fallback='')=='on'
    and prefs.get('XeFG','UIComposition',fallback='')=='0'
    and prefs.get('XeFG','SdkDeadlines',fallback='')=='0')
(d/'result.json').write_text(json.dumps({'latency_output':args.latency_output,'runtime_layout':args.runtime_layout,'runtime_selection_verified':runtime_selection,'inputs':manifest,'exit':result.returncode,
    'stdout':result.stdout,'stderr':result.stderr,'control_preferences_persisted':persisted,
    'sdk_policy_activation_verified':policy_activation,'sdk_deadline_records':len(sdk_deadlines),
    'sdk_deadline_passthrough_verified':passthrough,'hud_filter_verified':hud_filter,'passthrough_identity_verified':passthrough_identity,
    'native_reshade_keys_verified':native_keys,'native_reshade_child_verified':native_child,'foreground_reshade_verified':foreground_verified,
    'production_panel_verified':'Production panel draws=' in result.stdout},indent=2),encoding='utf-8')
print(result.stdout,end=''); print(result.stderr,end=''); print('Evidence:',d)
raise SystemExit(0 if result.returncode==0 and 'PASS:' in result.stdout and runtime_selection and persisted and policy_activation and passthrough is not False and hud_filter and passthrough_identity and native_keys is not False and native_child is not False and foreground_verified is not False else 1)
