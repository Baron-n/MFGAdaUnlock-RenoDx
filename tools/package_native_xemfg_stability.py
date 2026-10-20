"""Seal R1.10 with passing regression tests AND explicitly incomplete game probes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET
import zipfile
from write_build_manifest import compiler_inputs


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser()
    for name in ('output','evidence','previous-addon','official-addon','intel-sdk'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    root=Path(__file__).resolve().parent.parent
    def git(*args):
        return subprocess.check_output(['git',*args],cwd=root,text=True).strip()
    if git('status','--porcelain'):
        raise ValueError('Commit reviewed sources before sealing')
    if a.output.exists() or Path(str(a.output)+'.zip').exists():
        raise ValueError('Never overwrite a sealed candidate')
    build=root/'build/tests'
    addon=build/'xemfg/Release/renodx-mfgunlock-native.addon64'
    digest=sha(addon)
    if sha(a.previous_addon)!='bce5e0cef035e546ef02783a7efa282d3eee71615f08f94d6430924ae5f38ba3':
        raise ValueError('Expected sealed R1.9.5 rollback')
    if sha(a.official_addon)!='9715987f6323a2699dd5b7636412258a6b97a180a4d70c0f79d286714e2972e2':
        raise ValueError('Expected official 1.4.5 rollback')
    cases=ET.parse(root/'build/ctest-native-r1.10.xml').getroot().findall('.//testcase')
    if len(cases)!=112 or any(c.find('failure') is not None for c in cases):
        raise ValueError('Complete 112-case regression report required')
    skipped=[c.attrib['name'] for c in cases if c.find('skipped') is not None]
    if len(skipped)!=3:
        raise ValueError('Review changed optional coverage')
    last=(build/'Testing/Temporary/LastTest.log').read_text(encoding='utf-8')
    assets={
        'renodx-mfgunlock.addon64':addon,
        'symbols/renodx-mfgunlock-native.pdb':build/'xemfg/Release/renodx-mfgunlock-native.pdb',
        'rollback/renodx-mfgunlock-R1.9.5.addon64':a.previous_addon,
        'rollback/renodx-mfgunlock-1.4.5.addon64':a.official_addon,
        'stability-report.md':root/'docs/research/xemfg-stability-r1.10-2026-10-20.md',
        'analyze_xemfg_runtime.py':root/'tools/analyze_xemfg_runtime.py',
        'notices/MFGAdaUnlock-MIT.txt':root/'LICENSE',
        'notices/MFG-Unlocker-MIT.txt':root/'src/addons/mfgunlock/xemfg/vendor/LICENSE.dashdogy.txt',
        'notices/PROVENANCE.md':root/'src/addons/mfgunlock/xemfg/vendor/PROVENANCE.md',
        'notices/MinHook-LICENSE.txt':root/'src/addons/mfgunlock/xemfg/vendor/third_party/minhook/LICENSE.txt',
        'notices/Intel-program-data.txt':root/'src/addons/mfgunlock/xemfg/vendor/XEFG-PROGRAMS-NOTICE.txt',
        'notices/Intel-headers-MIT.txt':a.intel_sdk/'inc/MIT.txt',
        'notices/Intel-SDK-LICENSE.txt':a.intel_sdk/'LICENSE.txt',
        'evidence/ctest.xml':root/'build/ctest-native-r1.10.xml',
        'evidence/ctest.log':root/'build/ctest-native-r1.10.log',
        'evidence/last-test.log':build/'Testing/Temporary/LastTest.log',
        'evidence/baseline-inputs-manifest.json':a.evidence/'baseline/inputs-manifest.json',
        'evidence/tlou-baseline-summary.json':a.evidence/'tlou-baseline-summary.json',
        'evidence/requiem-baseline-summary.json':a.evidence/'requiem-baseline-summary.json',
    }
    for game in ('The Last of Us Part II','RESIDENT EVIL requiem BIOHAZARD requiem','OnimushaWotS','PRAGMATA'):
        assets['evidence/baseline/'+game+'/MFGAdaUnlock-XeMFG.log']=a.evidence/'baseline'/game/'MFGAdaUnlock-XeMFG.log'
    receipts={}
    def add_receipt(label,directory,expected=None):
        directory=Path(directory)
        result=json.loads((directory/'result.json').read_text())
        actual=result.get('inputs',{}).get('renodx-mfgunlock-native.addon64')
        if expected and actual!=expected:
            raise ValueError('Stale addon receipt: '+label)
        receipts[label]=dict(result_sha256=sha(directory/'result.json'),exit=result.get('exit'),
            timed_out=result.get('timed_out',False),addon_sha256=actual,
            inference_verified=result.get('inference_verified'),shutdown_completed=result.get('shutdown_completed'))
        for file in ('result.json','MFGAdaUnlock-XeMFG.log','ReShade.log','sl.log'):
            if (directory/file).is_file():
                assets['evidence/'+label+'/'+file]=directory/file
    for case in cases:
        name=case.attrib['name']
        if name in skipped: continue
        section=re.search(r'^\d+/\d+ Testing: '+re.escape(name)+r'\r?\n(.*?)(?=^\d+/\d+ Testing: |\Z)',last,re.M|re.S)
        found=re.findall(r'Evidence: ([^\r\n]+)',section[1] if section else case.findtext('system-out',''))
        if len(found)>1: raise ValueError('Ambiguous evidence: '+name)
        if found and (Path(found[0])/'result.json').is_file():
            result=json.loads((Path(found[0])/'result.json').read_text())
            expected=digest if 'renodx-mfgunlock-native.addon64' in result.get('inputs',{}) else None
            add_receipt(name,found[0],expected)
    required={'xemfg_native_historical_markers_XeLL','xemfg_native_historical_markers_Reflex'}
    required|={'xemfg_native_'+n for n in ('mixed-markers','alias-markers','stall-trace','queue-proxy','parallel-factory','foreign-refs')}
    if not required<=receipts.keys(): raise ValueError('Missing new regression receipts')
    extras=json.loads((a.evidence/'extra-validation.json').read_text())
    for label,receipt in extras.items():
        role=receipt.get('role','candidate')
        expected=sha(a.previous_addon) if role=='previous' else None if role=='stock' else digest
        add_receipt('probe-'+label,receipt['directory'],expected)
    negative=json.loads((Path(extras['historical_marker_negative']['directory'])/'result.json').read_text())
    if negative['exit']==0 or 'native output/Streamline/XeLL not ready' not in negative['stderr']:
        raise ValueError('Missing previous-build negative reproduction')
    a.output.mkdir(parents=True)
    for name,source in assets.items():
        dest=a.output/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
    (a.output/'README.txt').write_text(
        'MFG Unlock 1.4.5 + XeMFG R1.10 - LOCAL TEST BUILD\n\n'
        'Adds historical Reflex markers, guarded native queue/factory identity and serialized Intel context lifecycle.\n'
        '109 regression tests passed; 3 optional checks skipped. Genuine TLOU2 plugin probes are separate and incomplete: inference succeeded but process exit failed.\n'
        'TLOU2 gameplay, Steam-overlay compatibility and multi-second Capcom stalls are NOT confirmed fixed. Read stability-report.md.\n'
        'No new pacing/performance/physical latency gain claimed. No OptiScaler/GPL code copied. No runtime DLLs bundled.\n'
        'Close the game, back up the installed addon and replace only renodx-mfgunlock.addon64 in an existing native ReShade setup.\n'
        'Keep ReShade, RenoDX, INIs and the admitted Intel runtime pair. Do not run two FG transports together.\n'
        'Onimusha currently uses an OptiScaler DXGI loader locally; replacing just its addon does not migrate that setup.\n'
        'Do not change third-party overlay settings as part of installing this candidate. Steam compatibility remains unresolved.\n'
        'UI Composition stays optional and off by default; existing explicit preferences are retained.\n'
        'Rollback: close the game and restore its backup or the appropriate addon in rollback/. Never install both.\n\n'
        'Optional stall capture: add [Diagnostics] StallTrace=1 and GpuTiming=1 (each on its own line) to MFGAdaUnlock.ini before starting.\n'
        'Set both to 0 after diagnosis. GPU instrumentation adds queue work and log I/O: exclude it from performance comparisons.\n'
        'Analyze: python analyze_xemfg_runtime.py MFGAdaUnlock-XeMFG.log --pid <rendering PID> --output runtime-summary.json\n'
        'A source-queue fence does not identify private Intel kernel duration, global GPU idle, displayed cadence or physical latency.\n',encoding='utf-8')
    summary=dict(addon_sha256=digest,ctest=dict(total=112,passed=109,skipped=skipped,failed=0),receipts=receipts,
        gameplay_validated=False,steam_overlay_fixed=False,periodic_stalls_fixed=False,tlou2_gameplay_fixed=False)
    (a.output/'evidence/validation-summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    manifest=dict(identity='Native ReShade XeMFG R1.10',source_commit=git('rev-parse','HEAD'),
        branch=git('branch','--show-current'),base_commit=git('rev-parse','622b375'),addon_sha256=digest,
        official_1_4_5_commit='0fe65579d287581107babe267ee3e48c9135bf8a',
        optiscaler_dependency=False,runtime_dlls_bundled=False,diagnostics_default=False,
        gameplay_validated=False,steam_overlay_fixed=False,periodic_stalls_fixed=False,
        source_inputs_sha256={f.relative_to(root).as_posix():sha(f) for folder in ('src','tests','tools','docs')
            for f in sorted((root/folder).rglob('*')) if f.is_file() and '__pycache__' not in f.parts},
        compiler_inputs=compiler_inputs(build/'xemfg/mfgunlock_native_xemfg.vcxproj'),
        precompiled_inputs=json.loads((root/'build/xemfg-inputs/input-provenance.json').read_text()),
        assets=[dict(name=f.relative_to(a.output).as_posix(),sha256=sha(f),bytes=f.stat().st_size)
            for f in sorted(a.output.rglob('*')) if f.is_file()])
    (a.output/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    (a.output/'SHA256SUMS.txt').write_text(''.join(sha(f)+'  '+f.relative_to(a.output).as_posix()+'\n'
        for f in sorted(a.output.rglob('*')) if f.is_file()),encoding='utf-8')
    archive=Path(str(a.output)+'.zip')
    with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for f in sorted(a.output.rglob('*')):
            if f.is_file(): z.write(f,f.relative_to(a.output).as_posix())
    print(json.dumps(dict(output=str(a.output),addon_sha256=digest,archive_sha256=sha(archive)),indent=2))


if __name__=='__main__': main()
