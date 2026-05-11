#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${AEGIS_PRELOAD_LIB:-}" ]]; then
  echo "AEGIS_PRELOAD_LIB must point to libaegis.so" >&2
  exit 2
fi

if [[ "${AEGIS_CTEST_LIVE_SMOKE:-0}" == "1" &&
      "${AEGIS_RUN_LIVE_SMOKE:-0}" != "1" ]]; then
  echo "set AEGIS_RUN_LIVE_SMOKE=1 to run guarded live profiling smoke"
  exit 77
fi

if [[ -n "${AEGIS_PROFILE_REQUIREMENT:-}" ]]; then
  echo "${AEGIS_PROFILE_REQUIREMENT} smoke gate is wired but profiling parity is not implemented yet" >&2
  exit 1
fi

export AEGIS_LIVE_NOOPATCH="${AEGIS_LIVE_NOOPATCH:-1}"
export AEGIS_INSTRUMENTATION="${AEGIS_INSTRUMENTATION:-noopatch}"
export AEGIS_KERNEL_FILTER="${AEGIS_KERNEL_FILTER:-aegis_vector_add}"
export AEGIS_ARTIFACT_DIR="${AEGIS_ARTIFACT_DIR:-/tmp/aegis_new_aegis_artifacts}"
mkdir -p "${AEGIS_ARTIFACT_DIR}"

if [[ $# -eq 0 || "${1:-}" == "--build-and-run" ]]; then
  hipcc_bin="${HIPCC:-hipcc}"
  source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  smoke_bin="${AEGIS_SMOKE_BIN:-/tmp/aegis_vector_add_smoke}"
  "${hipcc_bin}" "${source_dir}/vector_add_smoke.hip" -O2 -o "${smoke_bin}"
  set -- "${smoke_bin}"
fi

LD_PRELOAD="${AEGIS_PRELOAD_LIB}${LD_PRELOAD:+:${LD_PRELOAD}}" "$@"

if [[ "${AEGIS_INSTRUMENTATION}" == "counting" ||
      "${AEGIS_INSTRUMENTATION}" == "countingPayload" ]]; then
  python3 - "${AEGIS_ARTIFACT_DIR}" <<'PY'
import glob
import json
import pathlib
import sys

artifact_dir = pathlib.Path(sys.argv[1])
records = []
for path in glob.glob(str(artifact_dir / "*profiling_records.json")):
    with open(path, "r", encoding="utf-8") as handle:
        records.extend(json.load(handle).get("records", []))

if not records:
    raise SystemExit("counting smoke did not produce profiling_records.json")
if not any(int(record.get("hitCount", 0)) > 0 for record in records):
    raise SystemExit("counting smoke did not produce a positive GPU record")
PY
fi
