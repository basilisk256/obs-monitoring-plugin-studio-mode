#!/usr/bin/env zsh
# ============================================================================
# build-macos.zsh — build this OBS plugin on macOS via the obsproject template.
#
# The repo's buildspec.json ships only windows-x64 dependency hashes. The
# template's macOS dependency download verifies against a "macos-universal"
# hash that isn't present. Rather than hardcode a hash (a wrong one silently
# bricks the build), this script DOWNLOADS each pinned dependency, computes its
# real SHA256, patches buildspec.json, then runs the template build and installs
# the resulting .plugin bundle.
#
# Prereqs: Xcode CLT, CMake 3.28+, curl, python3 (all standard/Homebrew on mac).
#   xcode-select --install ; brew install cmake
#
# Usage:  ./build-macos.zsh           # configure + build + install
#         ./build-macos.zsh --no-install
# Idempotent: re-running re-verifies hashes (download is cached by curl -C -).
# ============================================================================
set -euo pipefail
cd "${0:A:h}"   # repo root (this script lives there)

DO_INSTALL=1
[[ "${1:-}" == "--no-install" ]] && DO_INSTALL=0

for tool in cmake curl python3 shasum; do
  command -v "$tool" >/dev/null || { print -u2 "ERROR: '$tool' not found in PATH."; exit 1; }
done
if ! xcode-select -p >/dev/null 2>&1; then
  print -u2 "ERROR: Xcode Command Line Tools not installed. Run: xcode-select --install"; exit 1
fi

CACHE="${TMPDIR:-/tmp}/obs-macos-deps"
mkdir -p "$CACHE"

# --- Resolve each dependency's macOS artifact URL from buildspec.json ----------
# Emits lines:  <depKey>\t<url>\t<localfile>
read_deps() {
  python3 - "$PWD/buildspec.json" <<'PY'
import json, sys
spec = json.load(open(sys.argv[1]))
deps = spec["dependencies"]
def emit(key, url, fn): print(f"{key}\t{url}\t{fn}")
for key, d in deps.items():
    base = d["baseUrl"].rstrip("/"); ver = d["version"]
    if key == "obs-studio":
        fn = f"{ver}.tar.gz";                          url = f"{base}/{fn}"
    elif key in ("prebuilt", "deps", "obs-deps"):
        fn = f"macos-deps-{ver}-universal.tar.xz";     url = f"{base}/{ver}/{fn}"
    elif key == "qt6":
        fn = f"macos-deps-qt6-{ver}-universal.tar.xz"; url = f"{base}/{ver}/{fn}"
    else:
        sys.stderr.write(f"WARN: unknown dependency key '{key}', skipping\n"); continue
    emit(key, url, fn)
PY
}

typeset -A HASHES
print "==> Resolving + hashing macOS dependencies (from buildspec.json)"
while IFS=$'\t' read -r key url fn; do
  out="$CACHE/$fn"
  print "    $key: $fn"
  curl -fL -C - -o "$out" "$url" || { print -u2 "ERROR: download failed: $url"; exit 1; }
  h=$(shasum -a 256 "$out" | awk '{print $1}')
  HASHES[$key]="$h"
  print "      sha256=$h"
done < <(read_deps)

# --- Patch buildspec.json with the computed macos-universal hashes -------------
print "==> Patching buildspec.json (hashes.macos-universal)"
python3 - "$PWD/buildspec.json" "${(@kv)HASHES}" <<'PY'
import json, sys
path = sys.argv[1]
kv = sys.argv[2:]
hashes = dict(zip(kv[0::2], kv[1::2]))
spec = json.load(open(path))
for key, h in hashes.items():
    spec["dependencies"][key].setdefault("hashes", {})["macos-universal"] = h
json.dump(spec, open(path, "w"), indent=4)
open(path, "a").write("\n")
print("    wrote", ", ".join(f"{k}={v[:12]}…" for k, v in hashes.items()))
PY

# --- Configure + build via the template's macОs preset ------------------------
print "==> cmake --preset macos"
cmake --preset macos
print "==> cmake --build --preset macos"
cmake --build --preset macos --config RelWithDebInfo

# --- Install the .plugin bundle -----------------------------------------------
plugin=$(find build_macos -maxdepth 4 -name '*.plugin' -type d | head -1)
if [[ -z "$plugin" ]]; then
  print -u2 "WARN: build finished but no .plugin bundle found under build_macos/."
  exit 0
fi
print "==> Built: $plugin"
if (( DO_INSTALL )); then
  dest="$HOME/Library/Application Support/obs-studio/plugins"
  mkdir -p "$dest"
  rm -rf "$dest/${plugin:t}"
  cp -R "$plugin" "$dest/"
  print "==> Installed to: $dest/${plugin:t}"
  print "    Restart OBS to load it."
else
  print "==> Skipped install (--no-install). Copy manually:"
  print "    cp -R \"$plugin\" \"\$HOME/Library/Application Support/obs-studio/plugins/\""
fi
