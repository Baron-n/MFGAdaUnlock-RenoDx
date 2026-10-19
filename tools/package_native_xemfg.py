"""Package the independent ReShade transport, without OptiScaler/ASI/runtime DLLs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from write_build_manifest import compiler_inputs

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--official-addon',type=Path,required=True)
    ap.add_argument('--intel-sdk',type=Path,required=True)
    args=ap.parse_args()
    root=Path(__file__).resolve().parent.parent
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    build=root/'build/tests/xemfg'
    addon=build/'Release/renodx-mfgunlock-native.addon64'
    if b'Native ReShade XeMFG R1' not in addon.read_bytes():
        raise ValueError('Not the native ReShade binary')
    if sha(args.official_addon)!='9715987f6323a2699dd5b7636412258a6b97a180a4d70c0f79d286714e2972e2':
        raise ValueError('Rollback must be sealed official 1.4.5')
    assets={
        'renodx-mfgunlock.addon64':addon,
        'Install-NativeXeMFG.ps1':root/'tools/Install-NativeXeMFG.ps1',
        'Restore-MFGBackup.ps1':root/'tools/Restore-MFGBackup.ps1',
        'README.txt':root/'docs/native-xemfg-readme.txt',
        'engineering-report.md':root/'docs/research/native-reshade-xemfg-2026-10-19.md',
        'rollback/renodx-mfgunlock-1.4.5.addon64':args.official_addon,
        'notices/MFGAdaUnlock-MIT.txt':root/'LICENSE',
        'notices/MFG-Unlocker-MIT.txt':root/'src/addons/mfgunlock/xemfg/vendor/LICENSE.dashdogy.txt',
        'notices/MinHook-LICENSE.txt':root/'src/addons/mfgunlock/xemfg/vendor/third_party/minhook/LICENSE.txt',
        'notices/Intel-program-data.txt':root/'src/addons/mfgunlock/xemfg/vendor/XEFG-PROGRAMS-NOTICE.txt',
        'notices/Intel-headers-MIT.txt':args.intel_sdk/'inc/MIT.txt',
        'notices/Intel-SDK-LICENSE.txt':args.intel_sdk/'LICENSE.txt',
        'notices/PROVENANCE.md':root/'src/addons/mfgunlock/xemfg/vendor/PROVENANCE.md',
        'evidence/ctest.log':root/'build/ctest-native.log',
    }
    for count in (2,3,4,6):
        for case in sorted((root/'build/tests/native-checks').glob('native-*'),reverse=True):
            result=case/'result.json'; candidate=case/'renodx-mfgunlock-native.addon64'
            if not result.is_file() or not candidate.is_file() or sha(candidate)!=sha(addon): continue
            data=json.loads(result.read_text(encoding='utf-8'))
            if data['exit']!=0 or f'{count}x requested' not in data['stdout']: continue
            for name in ('result.json','MFGAdaUnlock-XeMFG.log','ReShade.log'):
                assets[f'evidence/{count}x/{name}']=case/name
            break
        else:
            raise ValueError(f'Missing final-addon {count}x evidence')
    for name,src in assets.items():
        dest=output/name; dest.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(src,dest)
    def git(*args):
        return subprocess.check_output(['git',*args],cwd=root,text=True).strip()
    source={}
    for folder in ('src','tests','tools','docs'):
        for path in sorted((root/folder).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                source[path.relative_to(root).as_posix()]=sha(path)
    manifest={
        'identity':'Native ReShade XeMFG R1',
        'source_commit':git('rev-parse','HEAD'),'branch':git('branch','--show-current'),
        'working_tree':git('status','--porcelain'),
        'official_1_4_5_commit':'0fe65579d287581107babe267ee3e48c9135bf8a',
        'previous_working_x1_3_commit':'0d8dc0b6d9aff9aad9afaf57f6939e94fada93c5',
        'license':'MIT bridge and reused MIT components; incorporated Intel data retains vendor terms',
        'optiscaler_dependency':False,'asi_dependency':False,
        'runtime_dlls_bundled':False,
        'intel_headers_commit':'de0fb9c1c510661c571164e1418ceca8101dab69',
        'upstream_mfg_commit':'a4cf2c4ab632e84c72254932793bb9e64b33d76e',
        'source_inputs_sha256':source,
        'compiler_inputs':compiler_inputs(build/'mfgunlock_native_xemfg.vcxproj'),
        'precompiled_inputs':json.loads((root/'build/xemfg-inputs/input-provenance.json').read_text(encoding='utf-8')),
        'test_scope':'50 passed, 3 optional skipped; real ReShade/Intel inference with synthetic Streamline inputs at 2x/3x/4x/6x; occluded output, no gameplay/display cadence/physical latency claim',
        'gameplay_validated':False,
        'assets':[{'name':name,'sha256':sha(output/name),'bytes':(output/name).stat().st_size} for name in assets],
    }
    (output/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    (output/'SHA256SUMS.txt').write_text(''.join(f'{sha(output/name)}  {name}\n' for name in [*assets,'manifest.json']),encoding='utf-8')
    print(output)

if __name__=='__main__': main()
