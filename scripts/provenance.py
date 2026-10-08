#!/usr/bin/env python3
"""Tie binary to source bytes without pretending cross-build is a runtime test."""
from pathlib import Path
import hashlib,json,subprocess,sys
root=Path(__file__).resolve().parents[1]
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
files=sorted(p for d in ('src','tests','resources','scripts') for p in (root/d).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
entries={str(p.relative_to(root)):digest(p) for p in files}
source=hashlib.sha256(json.dumps(entries,sort_keys=True,separators=(',',':')).encode()).hexdigest()
commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
dirty=bool(subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=root,text=True).strip())
if sys.argv[1]=='header':
 (root/'build'/'build_identity.h').write_text(f'#define QG_BUILD_COMMIT \"{commit}\"\n#define QG_BUILD_SOURCE \"{source}\"\n#define QG_BUILD_COMMIT_W L"{commit}"\n#define QG_BUILD_SOURCE_W L"{source}"\n#define QG_BUILD_DIRTY_W L"{str(dirty).lower()}"\n')
else:
 exe=root/'dist'/'QuietGuard.exe'
 out={'version':'0.3.1-disk-diagnostic','commit':commit,'tracked_tree_dirty_at_build':dirty,'source_tree_sha256':source,'source_files':entries,'exe_sha256':digest(exe),'exe_bytes':exe.stat().st_size,'native_windows_runtime_verified':False,'architecture':'Windows x86-64','compiler':subprocess.check_output(['x86_64-w64-mingw32-gcc','--version'],text=True).splitlines()[0]}
 (root/'dist'/'PROVENANCE.json').write_text(json.dumps(out,indent=2)+'\n')
 print(json.dumps({k:v for k,v in out.items() if k!='source_files'},indent=2))
