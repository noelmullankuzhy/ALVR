#!/bin/sh
# stdout is exactly one JSON result; diagnostics and logs go to stderr/scratch.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
scratch="$root/local-scratch/pc-build-streamer"
mkdir -p "$scratch"
started=$(date +%s)
commit=unknown
success=false
locked=false
finish() {
    rc=$?
    trap - EXIT
    if [ "$locked" = true ]; then
        git update-ref -d refs/lumen/pc-build-input >&2 || true
        rmdir "$scratch/lock"
    fi
    if [ "$success" != true ]; then
        python3 -c 'import json,sys,time; print(json.dumps(dict(ok=False,commit=sys.argv[1],seconds=int(time.time())-int(sys.argv[2]),error="Build failed; see local-scratch/pc-build-streamer logs"),separators=(",",":")))' "$commit" "$started"
    fi
    exit "$rc"
}
trap finish EXIT
[ "$#" -eq 1 ] || { printf 'Usage: sh tools/lumen/pc-build-streamer.sh <commit-ish>\n' >&2; exit 2; }
mkdir "$scratch/lock" || { printf 'Another build is active (scratch lock).\n' >&2; exit 1; }
locked=true
commit=$(git rev-parse --verify --end-of-options "$1^{commit}")
printf 'Building %s; logs: %s\n' "$commit" "$scratch" >&2
python3 - "$scratch/preflight.ps1" <<'PY'
import pathlib,sys
pathlib.Path(sys.argv[1]).write_text("""$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if ((Get-PSDrive C).Free -lt 60GB) { throw 'C: has less than 60 GB free' }
New-Item -ItemType Directory -Force 'C:\\Users\\noelm\\claude-tmp\\alvr-build\\incoming' | Out-Null
""", encoding='utf-8')
PY
pcps=/Users/noel/Projects/Personal/vr-gaming/tools/pcps.sh
sh "$pcps" "$scratch/preflight.ps1" >"$scratch/preflight.log" 2>&1 || { cat "$scratch/preflight.log" >&2; exit 1; }
git update-ref refs/lumen/pc-build-input "$commit"
git bundle create "$scratch/source.bundle" refs/lumen/pc-build-input >&2
scp -q "$scratch/source.bundle" tools/lumen/build-streamer-windows.ps1 win-workstation:C:/Users/noelm/claude-tmp/alvr-build/incoming/ >&2
python3 - "$scratch/run.ps1" "$commit" <<'PY'
import pathlib,sys
pathlib.Path(sys.argv[1]).write_text("& 'C:\\Users\\noelm\\claude-tmp\\alvr-build\\incoming\\build-streamer-windows.ps1' -Commit '"+sys.argv[2]+"'\n", encoding='utf-8')
PY
sh "$pcps" "$scratch/run.ps1" >"$scratch/remote.log" 2>&1 || { cat "$scratch/remote.log" >&2; exit 1; }
python3 - "$scratch/remote.log" "$scratch/result.json" "$started" "$commit" <<'PY'
import json,pathlib,sys,time
results=[]
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    try:
        value=json.loads(line)
        if isinstance(value,dict) and 'ok' in value: results.append(value)
    except ValueError: pass
if len(results)!=1 or not results[0]['ok'] or results[0]['commit']!=sys.argv[4]:
    raise SystemExit('Missing or invalid remote result; inspect remote.log')
result=results[0]
result['seconds']=int(time.time())-int(sys.argv[3])
line=json.dumps(result,separators=(',',':'))
pathlib.Path(sys.argv[2]).write_text(line+'\n')
print(line)
PY
success=true
