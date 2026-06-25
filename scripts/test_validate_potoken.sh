#!/usr/bin/env bash
#
# test_validate_potoken.sh
# ------------------------------------------------------------------
# Validates one or more YouTube video IDs against a running PoToken
# validation server.
#
#   GET  ${BASE_URL}/validate_potoken/<video_id>
#     -> 200  (empty body)            => token accepted   (PASS)
#     -> 401  {"error": "reason"}     => token rejected   (FAIL)
#     -> anything else                => unexpected       (ERROR)
#
# The validation server is expected to be running separately; this
# script only exercises the endpoint and reports results.
#
# Usage:
#   ./scripts/test_validate_potoken.sh [video_id ...]
#
# Environment:
#   BASE_URL   Base URL of the server  (default: http://localhost:3000)
#   TIMEOUT    curl max time, seconds  (default: 30)
#
# Examples:
#   ./scripts/test_validate_potoken.sh dQw4w9WgXcQ
#   BASE_URL=http://127.0.0.1:3000 ./scripts/test_validate_potoken.sh abc123 def456
#
# Exit code: 0 if every video ID passes, 1 otherwise.
# ------------------------------------------------------------------

set -u

BASE_URL="${BASE_URL:-http://localhost:3000}"
TIMEOUT="${TIMEOUT:-30}"

# Default sample set used when no video IDs are passed on the command line.
DEFAULT_VIDEO_IDS=(
    "dQw4w9WgXcQ"
)

# --- colors (disabled when not a tty) ------------------------------
if [ -t 1 ]; then
    C_RESET=$'\033[0m'; C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_YELLOW=$'\033[33m'; C_DIM=$'\033[2m'
else
    C_RESET=""; C_GREEN=""; C_RED=""; C_YELLOW=""; C_DIM=""
fi

command -v curl >/dev/null 2>&1 || { echo "error: curl is required" >&2; exit 2; }

# --- collect targets -----------------------------------------------
if [ "$#" -gt 0 ]; then
    VIDEO_IDS=("$@")
else
    VIDEO_IDS=("${DEFAULT_VIDEO_IDS[@]}")
    echo "${C_DIM}No video IDs given; using default set.${C_RESET}"
fi

echo "Server: ${BASE_URL}"
echo "------------------------------------------------------------------"

pass=0
fail=0
err=0

validate_one() {
    local video_id="$1"
    local url="${BASE_URL}/validate_potoken/${video_id}"

    # Capture body and the HTTP status code separately. The body and a
    # trailing "<newline>HTTP_STATUS:<code>" marker are written to stdout.
    local response http_code body
    response="$(curl -sS -m "${TIMEOUT}" -w $'\n__HTTP_STATUS__:%{http_code}' "${url}" 2>&1)"
    local curl_rc=$?

    if [ "${curl_rc}" -ne 0 ]; then
        printf '%s[ERROR]%s %-16s %s(could not reach server: %s)%s\n' \
            "${C_YELLOW}" "${C_RESET}" "${video_id}" "${C_DIM}" "${response%%$'\n'*}" "${C_RESET}"
        err=$((err + 1))
        return
    fi

    http_code="${response##*__HTTP_STATUS__:}"
    body="${response%$'\n'__HTTP_STATUS__:*}"

    case "${http_code}" in
        200)
            printf '%s[PASS]%s  %-16s %s200 OK%s\n' \
                "${C_GREEN}" "${C_RESET}" "${video_id}" "${C_DIM}" "${C_RESET}"
            pass=$((pass + 1))
            ;;
        401)
            # Body is expected to be {"error": "reason"} — extract the reason.
            local reason
            reason="$(printf '%s' "${body}" | sed -n 's/.*"error"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
            [ -z "${reason}" ] && reason="${body}"
            printf '%s[FAIL]%s  %-16s %s401 — %s%s\n' \
                "${C_RED}" "${C_RESET}" "${video_id}" "${C_DIM}" "${reason}" "${C_RESET}"
            fail=$((fail + 1))
            ;;
        *)
            printf '%s[ERROR]%s %-16s %sunexpected HTTP %s: %s%s\n' \
                "${C_YELLOW}" "${C_RESET}" "${video_id}" "${C_DIM}" "${http_code}" "${body}" "${C_RESET}"
            err=$((err + 1))
            ;;
    esac
}

for vid in "${VIDEO_IDS[@]}"; do
    validate_one "${vid}"
done

echo "------------------------------------------------------------------"
printf 'Total: %d   %sPass: %d%s   %sFail: %d%s   %sError: %d%s\n' \
    "${#VIDEO_IDS[@]}" \
    "${C_GREEN}" "${pass}" "${C_RESET}" \
    "${C_RED}"   "${fail}" "${C_RESET}" \
    "${C_YELLOW}" "${err}" "${C_RESET}"

# Non-zero exit if anything failed or errored.
if [ "${fail}" -gt 0 ] || [ "${err}" -gt 0 ]; then
    exit 1
fi
exit 0
