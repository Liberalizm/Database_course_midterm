#!/usr/bin/env bash
# Runs one layer of the test suite via CTest.
#
#   usage: ci/run-tests.sh <build-dir> <config> <label-regex>
#   e.g.   ci/run-tests.sh build Release unit
#
# Unlike a bare `ctest`, this fails when:
#   * a test fails or times out,
#   * no test matches the label (a mis-configured build must not look green),
#   * any test is reported as skipped (a crashed suite-level setup shows up as
#     "skipped" in CTest and would otherwise hide real failures).
set -uo pipefail

build_dir=${1:?build dir}
config=${2:?config}
label=${3:?label regex}
log="${build_dir}/ctest-$(printf '%s' "${label}" | tr -c 'a-zA-Z0-9' '_').log"

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

ctest --test-dir "${build_dir}" -C "${config}" -L "${label}" \
      --output-on-failure --no-tests=error --timeout 300 -j "${jobs}" 2>&1 | tee "${log}"
status=${PIPESTATUS[0]}

if grep -q "The following tests did not run" "${log}"; then
    echo "::error::Some '${label}' tests were skipped - treating as failure"
    exit 1
fi
exit "${status}"
