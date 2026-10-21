"""Run the native ReShade transport in a separate directory, never in a game."""
import argparse
import ctypes
import configparser
import datetime
import hashlib
import json
import re
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
ap.add_argument('--retire-sleep',action='store_true')
ap.add_argument('--producer-shadow',action='store_true')
ap.add_argument('--legacy-factory',action='store_true')
ap.add_argument('--legacy-markers',action='store_true')
ap.add_argument('--mixed-markers',action='store_true')
ap.add_argument('--alias-markers',action='store_true')
ap.add_argument('--stall-trace',action='store_true')
ap.add_argument('--queue-proxy',action='store_true')
ap.add_argument('--parallel-factory',action='store_true')
ap.add_argument('--foreign-refs',action='store_true')
ap.add_argument('--retained-output',action='store_true')
ap.add_argument('--secondary-viewport',action='store_true')
ap.add_argument('--normal-depth',action='store_true')
ap.add_argument('--hud-conflict',action='store_true')
ap.add_argument('--tail-sleep',action='store_true')
ap.add_argument('--steam-observer',type=Path)
ap.add_argument('--stale-input',action='store_true')
ap.add_argument('--active-support',action='store_true')
ap.add_argument('--zero-resource',action='store_true')
ap.add_argument('--jittered-motion',action='store_true')
ap.add_argument('--simulation-sleep',action='store_true')
ap.add_argument('--midsim-sleep',action='store_true')
ap.add_argument('--d3d-debug',action='store_true')
ap.add_argument('--reframework',choices=('detached','not-tracked','blocked','invalid','exception','late','legacy','ambiguous'))
ap.add_argument('--ref-v1',type=Path)
ap.add_argument('--ref-legacy',type=Path)
args=ap.parse_args()
d=args.directory.resolve()/datetime.datetime.now().strftime('native-%Y%m%d-%H%M%S-%f')
d.mkdir(parents=True)
executable_name='DS2.exe' if args.jittered_motion else args.exe.name
inputs={args.addon:'renodx-mfgunlock-native.addon64',args.exe:executable_name,
        args.fixture:'sl.interposer.dll',args.reshade:'dxgi.dll',
        args.sdk_fg:'libxess_fg.dll',args.sdk_ll:'libxell.dll'}
manifest={}
for src,name in inputs.items():
    shutil.copy2(src,d/name)
    manifest[name]=hashlib.sha256((d/name).read_bytes()).hexdigest()
if args.steam_observer:
    shutil.copy2(args.steam_observer,d/'gameoverlayrenderer64.dll')
    manifest['gameoverlayrenderer64.dll']=hashlib.sha256(args.steam_observer.read_bytes()).hexdigest()
if args.reframework:
    for src in (args.ref_v1,args.ref_legacy):
        if src is None: ap.error('REFramework cases require both fixture paths')
        shutil.copy2(src,d/src.name)
        manifest[src.name]=hashlib.sha256(src.read_bytes()).hexdigest()
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
(d/'MFGAdaUnlock.ini').write_text(f'[Backend]\nSelected=XeMFG\n[XeFG]\nMultiplier={args.multiplier}\n'
    +('InvertedDepth=0\n' if args.normal_depth else '')+f'[Latency]\nOutput={args.latency_output}\n',encoding='ascii')
if args.d3d_debug:
    with (d/'MFGAdaUnlock.ini').open('a') as f: f.write('[Diagnostics]\nStallTrace=1\n')
if args.retained_output:
    with (d/'MFGAdaUnlock.ini').open('a') as f: f.write('[Compatibility]\nRetainNativeOutput=1\n')
(d/'ReShade.ini').write_text('[GENERAL]\nNoDebugInfo=1\n[INPUT]\nInputProcessing=2\nKeyOverlay=36,0,0,0\n[OVERLAY]\nTutorialProgress=4\n',encoding='ascii')
ctypes.windll.kernel32.SetErrorMode(3)
flags=(['--overlay-foreground'] if args.overlay_foreground else ['--overlay-keys'] if args.overlay_keys else [])
if args.d3d_debug: flags+=['--d3d-debug']
if args.capcom_flags: flags+=['--capcom-flags']
if args.retire_sleep: flags+=['--retire-sleep']
if args.producer_shadow: flags+=['--producer-shadow']
if args.legacy_factory: flags+=['--legacy-factory']
if args.reframework: flags+=['--ref-'+args.reframework]
if args.steam_observer: flags+=['--steam-observer']
if args.stale_input: flags+=['--stale-input']
if args.active_support: flags+=['--active-support']
for name in ('legacy_markers','mixed_markers','alias_markers','stall_trace','queue_proxy','parallel_factory','foreign_refs','retained_output','secondary_viewport','normal_depth','hud_conflict','tail_sleep','zero_resource','jittered_motion','simulation_sleep','midsim_sleep'):
    if getattr(args,name): flags+=['--'+name.replace('_','-')]
if args.runtime_layout in ('preloaded-storage','mutated-storage'): flags+=['--preload-runtimes']
if args.runtime_layout in ('mutated-storage','missing'): flags+=['--expect-startup-rejection']
result=subprocess.run([str(d/executable_name),str(args.multiplier),*flags],cwd=d,capture_output=True,text=True,timeout=120,creationflags=0x08000000)
prefs=configparser.ConfigParser(); prefs.read(d/'MFGAdaUnlock.ini',encoding='ascii')
events=[json.loads(line) for line in (d/'MFGAdaUnlock-XeMFG.log').read_text(encoding='utf-8').splitlines()] if (d/'MFGAdaUnlock-XeMFG.log').exists() else []
if args.jittered_motion:
    refused='PASS: unsupported jittered-motion shader reported; native rendering/presentation preserved without Intel context' in result.stdout
    refused=refused and not any(e.get('event') in ('context_create','native_swapchain_init_failed','shader_decode_failure') for e in events)
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,
        jitter_unsupported_passthrough_verified=refused),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    raise SystemExit(0 if result.returncode==0 and refused else 1)
if args.reframework:
    blocked=args.reframework in ('blocked','invalid','exception','ambiguous')
    decisions=[e for e in events if e.get('event')=='native_reframework_retire']
    expected={'detached':2,'not-tracked':1,'blocked':3,'invalid':4,'exception':5,'late':2,'legacy':0,'ambiguous':7}[args.reframework]
    verified=bool(decisions) and all(e['decision']==expected for e in decisions)
    if blocked:
        verified=verified and 'PASS: REFramework refusal preserved proxy/context, blocked recreation and avoided retry' in result.stdout
        verified=verified and not any(e.get('event')=='native_output_retired' for e in events)
    else:
        verified=verified and 'REFramework safe handoff: reentrant callback, module lease and recreation verified' in result.stdout
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,scenario=args.reframework,retirement_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    raise SystemExit(0 if result.returncode==0 and verified else 1)
if args.parallel_factory:
    verified='PASS: simultaneous factory callers published one Intel owner; both outputs and contexts retired' in result.stdout
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,parallel_factory_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    raise SystemExit(0 if result.returncode==0 and verified else 1)
if args.foreign_refs or args.retained_output:
    verified='PASS: foreign native references block HWND recreation; only observer-owned releases permit recovery' in result.stdout
    if args.retained_output:
        verified='PASS: retained native HWND rebound to a new Intel context with observer references preserved' in result.stdout
        verified=verified and any(e.get('event')=='native_output_reused' and e.get('result')==0 for e in events)
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,foreign_reference_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    raise SystemExit(0 if result.returncode==0 and verified else 1)
if args.retire_sleep:
    verified=any(e.get('event')=='native_output_retired' and e.get('sleep_in_flight') and e.get('destroy_result')==0 for e in events)
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,retirement_during_sleep_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    raise SystemExit(0 if result.returncode==0 and verified and 'same HWND rebound before sleep returned' in result.stdout else 1)
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
legacy_verified=(('Legacy factory: inference, resize, fallback and recreation verified' in result.stdout
    and 'Legacy native fallback: FP16 unchanged; no Intel context or manual final overlay' in result.stdout) if args.legacy_factory else None)
markers_verified=('Historical markers: original calls preserved, real inference and duplicate forwarding verified' in result.stdout) if args.legacy_markers or args.mixed_markers or args.alias_markers else None
tail_verified=(('Tail sleep: first frame bypassed, following frames interpolated, original Reflex token and wait count preserved' in result.stdout)
    and any(e.get('event')=='native_sleep_association' and e.get('target')==e.get('source',0)+1 for e in events)) if args.tail_sleep else None
# Async diagnostics deliberately drop records rather than block a producer.
observer_verified=('Observer isolation: native plugin state preserved; game inference continued after observer OFF' in result.stdout) if args.steam_observer else None
stale_verified=('Stale source input: original return preserved; old epoch discarded before new-session admission' in result.stdout) if args.stale_input else None
input_verified={}
if args.zero_resource:
    input_verified['zero_resource']=('Zero resource metadata: real D3D12 textures, volatile producer copies and inference accepted' in result.stdout)
if args.jittered_motion:
    input_verified['jittered_motion']=('Jittered motion: observed executable default initialized the matching SDK convention' in result.stdout)
if args.simulation_sleep:
    recovered=[e for e in events if e.get('event')=='native_simulation_boundary_sleep']
    input_verified['simulation_sleep']=('Simulation boundary: missing sleep recovered after warmup; explicit source waits resumed without duplicate sleep' in result.stdout
        and bool(recovered) and all(e.get('result')==0 for e in recovered))
if args.midsim_sleep:
    recovered=[e for e in events if e.get('event')=='native_simulation_boundary_sleep']
    refused=[e for e in events if e.get('event')=='native_sleep_rejected']
    input_verified['midsim_sleep']=('Simulation boundary: refused mid-simulation waits recovered after warmup; explicit source waits resumed without duplicate sleep' in result.stdout
        and bool(recovered) and all(e.get('result')==0 for e in recovered)
        and bool(refused) and all(e.get('reason')=='inside_simulation' and e.get('observed',0)&3==1 for e in refused))
compatibility_verified=False
if (d/'compatibility.json').is_file():
    support=json.loads((d/'compatibility.json').read_text(encoding='utf-8'))
    compatibility_verified=(support.get('schema')=='mfgunlock.compatibility' and support.get('version')==1
        and support.get('displayed_frames_measured') is False and support.get('physical_latency_measured') is False
        and support.get('fallback')=='xemfg_selected_passthrough'
        and all(v in ('confirmed','inferred','unavailable','invalid') for v in support['capabilities'].values())
        and support['modules']['frame_generation'].get('sha256')==manifest['libxess_fg.dll']
        and support['modules']['latency'].get('sha256')==manifest['libxell.dll']
        and not re.search(r'[A-Za-z]:[\\/]',json.dumps(support)))
    if args.steam_observer: compatibility_verified=compatibility_verified and support.get('external_observer_resolutions')==2
    if args.stale_input: compatibility_verified=compatibility_verified and support.get('stale_inputs_discarded')==1
active_support_verified=None
if args.active_support:
    active=json.loads((d/'active-compatibility.json').read_text(encoding='utf-8')) if (d/'active-compatibility.json').is_file() else {}
    active_support_verified=(active.get('lifecycle')=='active' and active.get('blocker')=='ready'
        and active.get('frame',{}).get('id')==49
        and all(active.get('capabilities',{}).get(key)=='confirmed' for key in
                ('depth','motion_vectors','constants','source_wait','latency_markers'))
        and len(active.get('frame',{}).get('resource_receipts',[]))==3
        and all(r['accepted'] and r['width']==640 and r['height']==360 and r['rejection']=='none'
                for r in active.get('frame',{}).get('resource_receipts',[])))
# Diagnostics every native session must emit (plan items 1.2-1.8).
diagnostics_verified={}
session=[e for e in events if e.get('event')=='native_session']
inventory=[e for e in events if e.get('event')=='native_modules']
diagnostics_verified['session']=(len(session)==1 and re.match(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ',session[0].get('utc','')) is not None
    and session[0].get('executable')==executable_name and bool(session[0].get('gpu')) and session[0].get('windows')
    and bool(inventory) and any(m.get('name')=='sl.interposer.dll' for m in inventory[0].get('modules',[]))
    and not re.search(r'[A-Za-z]:[\\/]',json.dumps(session+inventory)))
# The child checks each real API call, SDK status and output lifetime directly;
# event counts are supporting evidence, not the acceptance oracle.
(d/'result.json').write_text(json.dumps({'latency_output':args.latency_output,'runtime_layout':args.runtime_layout,'runtime_selection_verified':runtime_selection,'inputs':manifest,'exit':result.returncode,
    'stdout':result.stdout,'stderr':result.stderr,'control_preferences_persisted':persisted,
    'sdk_policy_activation_verified':policy_activation,'sdk_deadline_records':len(sdk_deadlines),
    'sdk_deadline_passthrough_verified':passthrough,'hud_filter_verified':hud_filter,'passthrough_identity_verified':passthrough_identity,
    'native_reshade_keys_verified':native_keys,'native_reshade_child_verified':native_child,'foreground_reshade_verified':foreground_verified,
    'production_panel_verified':'Production panel draws=' in result.stdout,
    'producer_shadow_verified':args.producer_shadow and result.returncode==0 and 'Heap producer commands: accepted; non-code implementation: rejected; generation resumed' in result.stdout,
    'legacy_factory_verified':legacy_verified,
    'historical_markers_verified':markers_verified,
    'tail_sleep_verified':tail_verified,
    'observer_isolation_verified':observer_verified,
    'stale_input_verified':stale_verified,
    'input_compatibility_verified':input_verified,
    'compatibility_export_verified':compatibility_verified,
    'active_support_verified':active_support_verified,'diagnostics_verified':diagnostics_verified,
    'native_queue_identity_verified':('Queue native identity: wrapper unwrapped; inference and lifetime verified' in result.stdout) if args.queue_proxy else None,
    'gpu_queue_timestamps_verified':any(e.get('event')=='gpu_queue_span' for e in events) if args.stall_trace else None,
    'legacy_factory_success_records':sum(e.get('event')=='native_legacy_factory_create' and e.get('replaced') for e in events)},indent=2),encoding='utf-8')
print(result.stdout,end=''); print(result.stderr,end=''); print('Evidence:',d)
raise SystemExit(0 if result.returncode==0 and 'PASS:' in result.stdout and all(diagnostics_verified.values()) and compatibility_verified and all(input_verified.values()) and active_support_verified is not False and runtime_selection and persisted and policy_activation and passthrough is not False and hud_filter and passthrough_identity and native_keys is not False and native_child is not False and foreground_verified is not False and legacy_verified is not False and markers_verified is not False and tail_verified is not False and observer_verified is not False and stale_verified is not False and (not args.stall_trace or any(e.get('event')=='gpu_queue_span' for e in events)) and (not args.queue_proxy or 'Queue native identity: wrapper unwrapped; inference and lifetime verified' in result.stdout) else 1)
