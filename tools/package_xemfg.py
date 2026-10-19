"""Package the dual backend without bundling OptiScaler or Intel runtime DLLs."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--official-addon',type=Path,required=True)
    ap.add_argument('--intel-headers',type=Path,required=True)
    args=ap.parse_args()
    root=Path(__file__).resolve().parent.parent
    out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    native=root/'build/tests/xemfg/Release'
    assets={
        'renodx-mfgunlock.addon64':native/'renodx-mfgunlock-dual.addon64',
        'MFGAdaUnlock.XeMFG.asi':native/'MFGAdaUnlock.XeMFG.asi',
        'Switch-MFGBackend.ps1':root/'tools/Switch-MFGBackend.ps1',
        'Restore-MFGBackup.ps1':root/'tools/Restore-MFGBackup.ps1',
        'rollback/renodx-mfgunlock-1.4.5.addon64':args.official_addon,
        'notices/MFGAdaUnlock-LICENSE.txt':root/'LICENSE',
        'notices/MFG-Unlocker-MIT.txt':root/'src/addons/mfgunlock/xemfg/vendor/LICENSE.dashdogy.txt',
        'notices/MinHook-LICENSE.txt':root/'src/addons/mfgunlock/xemfg/vendor/third_party/minhook/LICENSE.txt',
        'notices/Intel-program-data.txt':root/'src/addons/mfgunlock/xemfg/vendor/XEFG-PROGRAMS-NOTICE.txt',
        'notices/Intel-headers-MIT.txt':args.intel_headers/'inc/MIT.txt',
        'notices/Intel-SDK-LICENSE.txt':args.intel_headers/'LICENSE.txt',
        'PROVENANCE.md':root/'src/addons/mfgunlock/xemfg/vendor/PROVENANCE.md',
        'engineering-report.md':root/'docs/research/dual-backend-xemfg-2026-10-19.md',
    }
    if sha(args.official_addon)!='9715987f6323a2699dd5b7636412258a6b97a180a4d70c0f79d286714e2972e2':
        raise ValueError('Rollback must be the sealed official 1.4.5 addon')
    for name,src in assets.items():
        target=out/name; target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(src,target)
    def git(*params):
        return subprocess.check_output(['git','-c','safe.directory='+str(root),*params],cwd=root,text=True).strip()
    source={}
    for folder in ['src','tests','tools','docs']:
        for p in sorted((root/folder).rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts:
                source[p.relative_to(root).as_posix()]=sha(p)
    evidence={}
    for name in ['build/ctest.log','build/runtime-smoke/final-smoke.log',
                 'build/runtime-smoke/MFGAdaUnlock-XeMFG.log',
                 'build/runtime-inactive/result.log','build/runtime-bad-provider/result.log']:
        p=root/name
        if p.exists():
            target=out/'evidence'/Path(name).parent.name/Path(name).name
            target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
            evidence[target.relative_to(out).as_posix()]=sha(target)
    manifest={
        'identity':'Dual Backend X1 - official 1.4.5 base',
        'baseline_commit':'0fe65579d287581107babe267ee3e48c9135bf8a',
        'source_commit':git('rev-parse','HEAD'),'branch':git('branch','--show-current'),
        'working_tree':git('status','--porcelain'),
        'upstream_mfg_commit':'a4cf2c4ab632e84c72254932793bb9e64b33d76e',
        'intel_headers_commit':'de0fb9c1c510661c571164e1418ceca8101dab69',
        'reference_optiscaler_commit':'c2e44a8bc911010d7ed3e4fa63843185fbb55d03',
        'assets':[{'name':name,'sha256':sha(out/name),'bytes':(out/name).stat().st_size} for name in assets],
        'source_inputs_sha256':source,'evidence_sha256':evidence,
        'precompiled_inputs':json.loads((root/'build/xemfg-inputs/input-provenance.json').read_text(encoding='utf-8')),
        'test_scope':'40 CTest passed; 3 optional historical/NVIDIA-input checks skipped. Real SDK lifecycle and synthetic host tests; no generated image or gameplay validation.',
        'gameplay_validated':False,
    }
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding='utf-8')
    lines=[sha(out/name)+'  '+name for name in assets]
    lines.append(sha(out/'manifest.json')+'  manifest.json')
    (out/'SHA256SUMS.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(out)


if __name__=='__main__': main()
