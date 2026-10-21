#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Runs a workflow step's command, keeps its output in $RUNNER_TEMP/<name>.log and, when it fails,
# reports the end of that output and the lines matching <pattern> as error annotations (visible
# through the public checks API without access to the job log). Bash counterpart of logged.ps1.
#
#   .github/scripts/logged.sh <name> '<pattern>' -- <command> [args...]
set -uo pipefail
name=$1; pattern=$2; shift 2; [ "${1:-}" = "--" ] && shift
log="${RUNNER_TEMP:-/tmp}/$name.log"
"$@" 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
[ "$rc" -eq 0 ] && exit 0
annotate() { # title, then the text on stdin
    local text
    text=$(cut -c1-400 | python3 -c 'import sys; t = sys.stdin.read(); print(t.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A"), end="")')
    echo "::error title=$1::$text"
}
tail -n 120 "$log" | annotate "$name failed (exit $rc) - last 120 lines"
if [ -n "$pattern" ]; then
    grep -E -- "$pattern" "$log" | head -n 80 | annotate "$name failed - lines matching $pattern"
fi
exit "$rc"
