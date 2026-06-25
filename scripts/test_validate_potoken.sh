#!/usr/bin/env bash
#
# test_validate_potoken.sh
# ------------------------------------------------------------------
# For each YouTube video ID:
#   1. Calls epotoken_cli to generate a po_token and placeholder_po_token
#      (visitor_data is the YouTube session identifier; video_id is used
#       as the content_binding so the token is bound to that video).
#   2. POSTs the tokens as JSON to the validation server:
#        POST ${BASE_URL}/validate_potoken/<video_id>
#        Content-Type: application/json
#        {"po_token":"<token>","placeholder_po_token":"<placeholder>"}
#   3. Interprets the response contract:
#        200 (empty body)        => token accepted  (PASS)
#        401 {"error":"reason"}  => token rejected  (FAIL)
#        anything else           => unexpected      (ERROR)
#
# The validation server must be running separately; this script only
# drives the generation + submission + result reporting.
#
# Required environment:
#   VISITOR_DATA   YouTube visitor data string for token generation
#
# Optional environment:
#   CLI_BIN    Path to the epotoken_cli binary
#              (default: ./build/epotoken_cli)
#   BASE_URL   Validation server base URL
#              (default: http://localhost:3000)
#   TIMEOUT    curl max time in seconds  (default: 30)
#
# Usage:
#   VISITOR_DATA="Cgt..." ./scripts/test_validate_potoken.sh [video_id ...]
#
# Examples:
#   VISITOR_DATA="Cgt..." ./scripts/test_validate_potoken.sh dQw4w9WgXcQ
#   VISITOR_DATA="Cgt..." ./scripts/test_validate_potoken.sh abc123 def456
#   VISITOR_DATA="Cgt..." CLI_BIN=./build/Release/epotoken_cli \
#       BASE_URL=http://127.0.0.1:3000 ./scripts/test_validate_potoken.sh abc123
#
# Exit code: 0 if every video ID passes, 1 otherwise.
# ------------------------------------------------------------------

set -u

BASE_URL="${BASE_URL:-http://localhost:3000}"
CLI_BIN="${CLI_BIN:-./build/epotoken_cli}"
TIMEOUT="${TIMEOUT:-30}"

# Default sample set when no video IDs are supplied.
DEFAULT_VIDEO_IDS=("dQw4w9WgXcQ")

# --- colours (disabled when stdout is not a tty) -------------------
if [ -t 1 ]; then
    C_RESET=$'\033[0m'
    C_GREEN=$'\033[32m'
    C_RED=$'\033[31m'
    C_YELLOW=$'\033[33m'
    C_CYAN=$'\033[36m'
    C_DIM=$'\033[2m'
else
    C_RESET=""; C_GREEN=""; C_RED=""; C_YELLOW=""; C_CYAN=""; C_DIM=""
fi

# --- prerequisite checks -------------------------------------------
if [ -z "${VISITOR_DATA:-}" ]; then
    echo "error: VISITOR_DATA env var is required" >&2
    echo "  e.g. VISITOR_DATA=\"Cgt...\" $0 <video_id>" >&2
    exit 2
fi
command -v curl >/dev/null 2>&1 || { echo "error: curl is required" >&2; exit 2; }
if [ ! -x "${CLI_BIN}" ]; then
    echo "error: CLI binary not found or not executable: ${CLI_BIN}" >&2
    echo "  Build with: cmake -B build -DBUILD_CLI=ON && cmake --build build" >&2
    echo "  Or set CLI_BIN=/path/to/epotoken_cli" >&2
    exit 2
fi

# --- collect targets -----------------------------------------------
if [ "$#" -gt 0 ]; then
    VIDEO_IDS=("$@")
else
    VIDEO_IDS=("${DEFAULT_VIDEO_IDS[@]}")
    echo "${C_DIM}No video IDs given; using default set.${C_RESET}"
fi

echo "Server:  ${BASE_URL}"
echo "CLI:     ${CLI_BIN}"
echo "--------------------------------------------------------------------"

pass=0
fail=0
err=0

# Extract the value of a "key: value" line from epotoken_cli output.
# Usage: parse_field "po_token" "$cli_output"
parse_field() {
    local field="$1" output="$2"
    printf '%s' "${output}" \
        | grep "^${field}:" \
        | sed "s/^${field}:[[:space:]]*//"
}

validate_one() {
    local video_id="$1"
    local url="${BASE_URL}/validate_potoken/${video_id}"

    # --- 1. Generate tokens via the CLI --------------------------------
    local cli_output cli_rc
    cli_output="$("${CLI_BIN}" "${VISITOR_DATA}" "${video_id}" 2>&1)"
    cli_rc=$?

    if [ "${cli_rc}" -ne 0 ]; then
        printf '%s[ERROR]%s %-16s %s(token generation failed: %s)%s\n' \
            "${C_YELLOW}" "${C_RESET}" "${video_id}" "${C_DIM}" \
            "${cli_output%%$'\n'*}" "${C_RESET}"
        err=$((err + 1))
        return
    fi

    local po_token placeholder_po_token
    po_token="$(parse_field "po_token" "${cli_output}")"
    placeholder_po_token="$(parse_field "placeholder_po_token" "${cli_output}")"

    if [ -z "${po_token}" ]; then
        printf '%s[ERROR]%s %-16s %s(po_token missing from CLI output)%s\n' \
            "${C_YELLOW}" "${C_RESET}" "${video_id}" "${C_DIM}" "${C_RESET}"
        err=$((err + 1))
        return
    fi

    # --- 2. Build JSON payload -----------------------------------------
    # Tokens are URL-safe base64 ([A-Za-z0-9_-]) so no JSON escaping needed.
    local payload
    payload="$(printf '{"po_token":"%s","placeholder_po_token":"%s"}' \
        "${po_token}" "${placeholder_po_token}")"

    # --- 3. POST to validation server ----------------------------------
    local response http_code body
    response="$(curl -sS -m "${TIMEOUT}" \
        -X POST "${url}" \
        -H "Content-Type: application/json" \
        -d "${payload}" \
        -w $'\n__HTTP_STATUS__:%{http_code}' 2>&1)"
    local curl_rc=$?

    if [ "${curl_rc}" -ne 0 ]; then
        printf '%s[ERROR]%s %-16s %s(could not reach server: %s)%s\n' \
            "${C_YELLOW}" "${C_RESET}" "${video_id}" "${C_DIM}" \
            "${response%%$'\n'*}" "${C_RESET}"
        err=$((err + 1))
        return
    fi

    http_code="${response##*__HTTP_STATUS__:}"
    body="${response%$'\n'__HTTP_STATUS__:*}"

    # --- 4. Interpret response -----------------------------------------
    case "${http_code}" in
        200)
            printf '%s[PASS]%s  %-16s %s200 OK  po_token=%.24s...%s\n' \
                "${C_GREEN}" "${C_RESET}" "${video_id}" "${C_DIM}" \
                "${po_token}" "${C_RESET}"
            pass=$((pass + 1))
            ;;
        401)
            local reason
            reason="$(printf '%s' "${body}" \
                | sed -n 's/.*"error"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
            [ -z "${reason}" ] && reason="${body}"
            printf '%s[FAIL]%s  %-16s %s401 — %s%s\n' \
                "${C_RED}" "${C_RESET}" "${video_id}" "${C_DIM}" \
                "${reason}" "${C_RESET}"
            fail=$((fail + 1))
            ;;
        *)
            printf '%s[ERROR]%s %-16s %sunexpected HTTP %s: %s%s\n' \
                "${C_YELLOW}" "${C_RESET}" "${video_id}" "${C_DIM}" \
                "${http_code}" "${body}" "${C_RESET}"
            err=$((err + 1))
            ;;
    esac
}

for vid in "${VIDEO_IDS[@]}"; do
    validate_one "${vid}"
done

echo "--------------------------------------------------------------------"
printf 'Total: %d   %sPass: %d%s   %sFail: %d%s   %sError: %d%s\n' \
    "${#VIDEO_IDS[@]}" \
    "${C_GREEN}" "${pass}"  "${C_RESET}" \
    "${C_RED}"   "${fail}"  "${C_RESET}" \
    "${C_YELLOW}" "${err}"  "${C_RESET}"

if [ "${fail}" -gt 0 ] || [ "${err}" -gt 0 ]; then
    exit 1
fi
exit 0
