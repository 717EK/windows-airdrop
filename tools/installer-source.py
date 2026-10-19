"""Capture corresponding source and a deterministic integrity manifest; exclude user data."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

p = argparse.ArgumentParser()
p.add_argument('--stage', type=Path, required=True)
p.add_argument('--deps', type=Path, required=True)
args = p.parse_args()
repo = Path(__file__).resolve().parents[1]
extensions = {'.c', '.h', '.cpp', '.hpp', '.cs', '.csproj', '.cmake', '.patch', '.ps1', '.py', '.sh', '.md', '.txt', '.json', '.in', '.iss', '.manifest', '.xml', '.rc', '.sln', '.vcxproj', '.yml', '.yaml', '.svg', '.png', '.ico', '.bmp', '.html'}
skip = {'.git', 'build', 'bin', 'obj', '__pycache__', 'node_modules', '.venv', 'venv'}
local_roots = {'airdrop', 'assets', 'compat', 'desktop', 'installer', 'licenses', 'netif', 'patches', 'radio', 'sched', 'tests', 'tools', '.github'}
local_files = {'CMakeLists.txt', '.gitattributes', '.gitignore', 'README.md', 'QUICK-START.html', 'LICENSE', 'COPYING', 'THIRD_PARTY_NOTICES.md', 'SECURITY.md', 'CONTRIBUTING.md', 'PRIVACY.md', 'CODE-SIGNING.md'}
public_docs = {'docs/build.md', 'docs/architecture.md', 'docs/limitations.md', 'docs/signpath-readiness.md'}
for name, root in [('PulseDrop', repo), ('OWL', repo / 'build/_deps/owl-src'), ('devourer', args.deps / 'devourer')]:
    with zipfile.ZipFile(args.stage / 'source' / (name + '.zip'), 'w', zipfile.ZIP_DEFLATED) as archive:
        for source in root.rglob('*'):
            relative = source.relative_to(root)
            if not source.is_file() or any(part in skip for part in relative.parts):
                continue
            # Local handovers and test captures are not corresponding source.
            if name == 'PulseDrop' and not (relative.parts[0] in local_roots or relative.as_posix() in local_files | public_docs):
                continue
            if (source.suffix in extensions or source.name in {'COPYING', 'LICENSE', 'CMakeLists.txt', 'Makefile', 'NOTICE', 'COPYING-LGPL'}
                    or (name == 'PulseDrop' and relative.as_posix() in local_files)):
                archive.write(source, relative.as_posix())
files = {}
for file in sorted(args.stage.rglob('*')):
    if (file.is_file() and file.name != 'payload-sha256.json' and file.suffix != '.pdb'
            and file.name != 'self-test.txt' and not file.name.endswith('-test.txt')
            and not file.name.startswith('preview')):
        with file.open('rb') as data:
            files[str(file.relative_to(args.stage))] = hashlib.file_digest(data, 'sha256').hexdigest()
(args.stage / 'payload-sha256.json').write_text(json.dumps(files, indent=2), encoding='utf-8')
