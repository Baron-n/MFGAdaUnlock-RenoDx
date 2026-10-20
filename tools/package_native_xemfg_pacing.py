"""Seal a native candidate and exact local validation; bundle no runtime DLLs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET
import zipfile
from write_build_manifest import compiler_inputs


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--revision', choices=('R1.8', 'R1.9'), default='R1.8')
    for name in ('output', 'evidence', 'previous-addon', 'official-addon', 'intel-sdk'):
        ap.add_argument('--' + name, type=Path, required=True)
    a = ap.parse_args()
    compat = a.revision == 'R1.9'
    revision = a.revision
    previous = 'R1.8' if compat else 'R1.7.5'
    total = 89 if compat else 80
    report_name = 'compatibility-cards-report.md' if compat else 'pacing-reflex-report.md'
    root = Path(__file__).resolve().parent.parent
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=root, text=True).strip()
    if git('status', '--porcelain'):
        raise ValueError('Commit the reviewed source before sealing')
    addon = root / 'build/tests/xemfg/Release/renodx-mfgunlock-native.addon64'
    digest = sha(addon)
    validation = json.loads((a.evidence / 'validation-summary.json').read_text(encoding='utf-8'))
    if validation['addon_sha256'] != digest or validation['ctest'] != dict(total=total, passed=total-3, skipped=3, failed=0):
        raise ValueError('Missing matching complete suite')
    xml = ET.parse(root / f'build/ctest-native-{revision.lower()}.xml').getroot()
    cases = xml.findall('.//testcase')
    if len(cases) != total or any(c.find('failure') is not None for c in cases):
        raise ValueError('CTest XML differs from validation')
    expected = {f'{route}-{m}x-61aeeab7' for route in ('XeLL', 'Reflex') for m in (2, 3, 4, 6)}
    expected |= {'XeLL-4x-d2030dcd', 'Reflex-4x-d2030dcd'}
    if set(validation['sdk_cases']) != expected:
        raise ValueError('Incomplete SDK / latency matrix')
    for key, receipt in validation['sdk_cases'].items():
        result = json.loads((Path(receipt['directory']) / 'result.json').read_text())
        if result['exit'] or result['inputs']['renodx-mfgunlock-native.addon64'] != digest:
            raise ValueError('Stale SDK evidence: ' + key)
        if not all(result[k] for k in ('control_preferences_persisted', 'sdk_policy_activation_verified', 'hud_filter_verified')):
            raise ValueError('SDK control contract failed: ' + key)
        route = key.split('-')[0]
        if result['latency_output'] != route or f'Latency route: Reflex -> {route};' not in result['stdout']:
            raise ValueError('Latency route not witnessed: ' + key)
    if compat:
        expected_layouts = {'storage-only', 'preloaded-storage', 'mutated-storage', 'missing', 'preloaded-storage-Reflex'}
        if set(validation['compat_cases']) != expected_layouts:
            raise ValueError('Incomplete runtime layout coverage')
        for layout, receipt in validation['compat_cases'].items():
            result = json.loads((Path(receipt['directory']) / 'result.json').read_text())
            if result['exit'] or result['inputs']['renodx-mfgunlock-native.addon64'] != digest:
                raise ValueError('Stale compatibility evidence: ' + layout)
            fields = ('startup_rejection_verified', 'model_unpublished') if layout in ('mutated-storage', 'missing') else ('runtime_selection_verified', 'control_preferences_persisted', 'hud_filter_verified')
            if not all(result.get(k) for k in fields):
                raise ValueError('Compatibility contract failed: ' + layout)
    previous_hash = '6825a8e2d7a3f5d57899f24db192be9b3bc68d3196e8d402760b7d8da577cf42' if compat else 'e054d4919753de2431a3259048c689982d6e5cc6d9f8d7f0b972179bdef14080'
    if sha(a.previous_addon) != previous_hash:
        raise ValueError(previous + ' rollback differs')
    if sha(a.official_addon) != '9715987f6323a2699dd5b7636412258a6b97a180a4d70c0f79d286714e2972e2':
        raise ValueError('Official rollback differs')
    if a.output.exists() or Path(str(a.output) + '.zip').exists():
        raise ValueError('Refuse to overwrite a sealed package')
    assets = {
        'renodx-mfgunlock.addon64': addon,
        report_name: root / ('docs/research/xemfg-r1.9-compatibility-cards-2026-10-20.md' if compat else 'docs/research/xemfg-r1.8-pacing-reflex-2026-10-20.md'),
        f'rollback/renodx-mfgunlock-{previous.lower()}.addon64': a.previous_addon,
        'rollback/renodx-mfgunlock-1.4.5.addon64': a.official_addon,
        'Restore-MFGBackup.ps1': root / 'tools/Restore-MFGBackup.ps1',
        'notices/MFGAdaUnlock-MIT.txt': root / 'LICENSE',
        'notices/MFG-Unlocker-MIT.txt': root / 'src/addons/mfgunlock/xemfg/vendor/LICENSE.dashdogy.txt',
        'notices/PROVENANCE.md': root / 'src/addons/mfgunlock/xemfg/vendor/PROVENANCE.md',
        'notices/MinHook-LICENSE.txt': root / 'src/addons/mfgunlock/xemfg/vendor/third_party/minhook/LICENSE.txt',
        'notices/Intel-program-data.txt': root / 'src/addons/mfgunlock/xemfg/vendor/XEFG-PROGRAMS-NOTICE.txt',
        'notices/Intel-headers-MIT.txt': a.intel_sdk / 'inc/MIT.txt',
        'notices/Intel-SDK-LICENSE.txt': a.intel_sdk / 'LICENSE.txt',
        'evidence/validation-summary.json': a.evidence / 'validation-summary.json',
        'evidence/ctest.log': root / f'build/ctest-native-{revision.lower()}.log',
        'evidence/ctest.xml': root / f'build/ctest-native-{revision.lower()}.xml',
        'evidence/last-test.log': root / 'build/tests/Testing/Temporary/LastTest.log',
    }
    if compat:
        assets['Install-NativeXeMfgLocal.ps1'] = root / 'tools/Install-NativeXeMfgLocal.ps1'
    for label, receipt in (validation['sdk_cases'] | validation.get('compat_cases', {})).items():
        directory = Path(receipt['directory'])
        for name in ('result.json', 'MFGAdaUnlock-XeMFG.log', 'ReShade.log'):
            assets[f'evidence/{label}/{name}'] = directory / name
    for page in range(4):
        assets[f'evidence/ui/page-{page}.png'] = root / f'build/tests/ui-preview/page-{page}.png'
    a.output.mkdir(parents=True)
    for name, source in assets.items():
        dest = a.output / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
    readme = (
        'MFG Unlock 1.4.5 + XeMFG R1.8 - local candidate\n\n'
        'New: XeMFG with Reflex -> Reflex or Reflex -> XeLL; driver-metered queue-ahead; CUDA PSO recorder recognition.\n'
        'Reflex output: enable Reflex in game; Latency > Advanced latency routing > Latency output > Reflex; restart.\n'
        'Auto still uses XeLL. UI Composition remains optional and off by default.\n'
        'General > Advanced pacing comparison > Queue metered frames early compares the new and previous CPU timing.\n'
        'Queue-ahead requires accepted driver metering and sync interval zero. All SDK resource waits remain.\n\n'
        'Close the game and back up the installed addon before replacing only renodx-mfgunlock.addon64.\n'
        'Keep ReShade, RenoDX, Intel runtime DLLs and INIs. No OptiScaler or ASI dependency. No runtime DLLs included.\n'
        '77 tests passed; 3 optional historical checks skipped. Numerical CUDA coverage: 9/17 layer programs.\n'
        'Real SDK tests prove dispatch and ABI routing; no gameplay FPS, displayed pacing or physical latency gain is claimed.\n'
        'The 1.6.2 beta was inspected statically; this is not a complete port of its unpublished changes.\n'
        'Read pacing-reflex-report.md for profile limitations and evidence.\n'
        'Rollback: close the game and restore R1.7.5 or official 1.4.5 from rollback; install only one MFG addon.\n')
    if compat:
        readme = (f'MFG Unlock 1.4.5 + XeMFG {revision} - local candidate\n\n'
            'Compact cards restored. Runtime selection follows the exact admitted loaded pair, including _storage_.\n'
            'Unknown or missing Intel pairs remain refused; the panel now explains startup failures.\n'
            'Reflex -> Reflex and Reflex -> XeLL from R1.8 remain available. Restart after selecting the output.\n'
            'UI Composition remains optional and off by default. No OptiScaler or ASI dependency.\n'
            'Supported XeFG: 1.3.1.78; XeLL: 1.3.0.5 or 1.3.2.10, admitted by exact hashes.\n'
            'No runtime DLLs included. The local installer can refresh the root and existing _storage_ copies with backup.\n'
            '86 tests passed; 3 optional historical checks skipped. Real SDK layout tests include mutated/missing runtime rejection.\n'
            'Gameplay activation in each game still requires a fresh scene/log; no new pacing or latency gain is claimed.\n'
            f'Read {report_name}. Rollback addon: {previous} or official DLSS-G 1.4.5 in rollback/.\n'
            'For game runtime rollback, use Restore-MFGBackup.ps1 with the installer backup directory.\n')
    (a.output / 'README.txt').write_text(readme, encoding='utf-8')
    source = {p.relative_to(root).as_posix(): sha(p) for folder in ('src', 'tests', 'tools', 'docs')
              for p in sorted((root / folder).rglob('*')) if p.is_file() and '__pycache__' not in p.parts}
    manifest = dict(identity='Native ReShade XeMFG '+revision, revision=revision, source_commit=git('rev-parse', 'HEAD'),
        branch=git('branch', '--show-current'), base_commit='8f7a45eedb5a54f4c29af1420facc8216fe8ca77' if compat else 'd3588a20bc950c0f13fcba627fd4d2c6d40c8088',
        official_1_4_5_commit='0fe65579d287581107babe267ee3e48c9135bf8a',
        addon_sha256=digest, optiscaler_dependency=False, asi_dependency=False, runtime_dlls_bundled=False,
        gameplay_validated=False, displayed_pacing_gain_measured=False, physical_latency_gain_measured=False,
        cuda_numerical_layer_coverage=dict(available=9, total=17),
        upstream_cuda_commit='29504cdd5d209a25e3b91e809a7e0d6f9db8cbdc',
        beta_asi_sha256='f34abd14975a5328ae3baf613f681b75d3075a4ff2a80da20cfefbaa0e9fab7b',
        beta_code_imported=False, source_inputs_sha256=source,
        compiler_inputs=compiler_inputs(root / 'build/tests/xemfg/mfgunlock_native_xemfg.vcxproj'),
        precompiled_inputs=json.loads((root / 'build/xemfg-inputs/input-provenance.json').read_text(encoding='utf-8')),
        assets=[dict(name=p.relative_to(a.output).as_posix(), sha256=sha(p), bytes=p.stat().st_size)
                for p in sorted(a.output.rglob('*')) if p.is_file()])
    (a.output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    (a.output / 'SHA256SUMS.txt').write_text(''.join(f'{sha(p)}  {p.relative_to(a.output).as_posix()}\n'
        for p in sorted(a.output.rglob('*')) if p.is_file()), encoding='utf-8')
    archive = Path(str(a.output) + '.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(a.output.rglob('*')):
            if p.is_file():
                z.write(p, p.relative_to(a.output).as_posix())
    print(json.dumps(dict(directory=str(a.output), archive=str(archive), zip_sha256=sha(archive), addon_sha256=digest), indent=2))


if __name__ == '__main__':
    main()
