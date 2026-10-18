"""Check artifact identity and provenance against the actual experimental build."""
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from write_build_manifest import make_manifest, sha

addon, project = map(Path, sys.argv[1:])
if not addon.is_file() or not project.is_file():
    print('SKIP: build the experimental addon before artifact provenance tests')
    raise SystemExit(77)
manifest = make_manifest(addon, ROOT, 'engineering-experimental', build_project=project)
assert manifest['baseline_commit'] == '3e898157fd59ed9eef65c181619060ddb305c6bd'
assert manifest['addon_sha256'] == sha(addon.read_bytes())
assert manifest['confidence_history'] is False and manifest['temporal_launch_hooks'] is False
assert 'src/addons/mfgunlock/kernel_identities.hpp' in manifest['source_inputs']
assert 'tools/write_build_manifest.py' in manifest['source_inputs']
assert manifest['build_project_sha256'] == sha(project.read_bytes())
assert manifest['instruction_profiles']['kernel_identities']
assert any(x.lower().endswith('detours.lib') for x in manifest['compiler_inputs']['files'])
assert any(x.lower().endswith('sl_dlss_g.h') for x in manifest['compiler_inputs']['files'])


def rejected(call):
    try:
        call()
    except ValueError:
        return
    raise AssertionError('Invalid artifact provenance was accepted')


rejected(lambda: make_manifest(addon, ROOT, 'release-1.4.4'))
rejected(lambda: make_manifest(addon, ROOT, 'engineering-experimental'))
with tempfile.TemporaryDirectory() as temporary:
    wrong = Path(temporary) / 'wrong.vcxproj'
    wrong.write_text('<Project/>', encoding='utf-8')
    rejected(lambda: make_manifest(addon, ROOT, 'engineering-experimental', build_project=wrong))
print('Actual experimental binary, baseline, flags, payloads and wrong-label rejection passed')
