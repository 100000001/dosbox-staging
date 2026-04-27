#!/usr/bin/env bash
# Hits every MCP tool against a running dosbox-mcp and prints a one-line
# verdict per call. Exits non-zero if any tool returns isError=true or a
# non-2xx HTTP status.
#
# Usage:
#   scripts/mcp-smoke-test.sh                    # default 127.0.0.1:4747
#   MCP_HOST=192.168.1.5 MCP_PORT=4747 ...       # override target
#
# Requires curl + python3.

set -u

HOST="${MCP_HOST:-127.0.0.1}"
PORT="${MCP_PORT:-4747}"
URL="http://${HOST}:${PORT}/mcp"

py() { python3 -c "$1"; }

# Initialize and capture session id from the response header.
init_resp_headers=$(mktemp)
init_resp_body=$(mktemp)
trap 'rm -f "$init_resp_headers" "$init_resp_body"' EXIT

curl -sS --max-time 5 -X POST "$URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json,text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}' \
  -D "$init_resp_headers" -o "$init_resp_body"

SID=$(awk -F': ' 'tolower($1)=="mcp-session-id"{gsub(/[\r\n]/,"",$2); print $2; exit}' "$init_resp_headers")
if [ -z "$SID" ]; then
  echo "FAIL: no Mcp-Session-Id in initialize response. Body:" >&2
  cat "$init_resp_body" >&2
  exit 1
fi
echo "init           ok session=$SID"

curl -sS --max-time 5 -X POST "$URL" \
  -H "Content-Type: application/json" -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' -o /dev/null

# Discover which tools the running server actually has, so we can skip
# tools added in a newer build than the running binary.
tools_body=$(curl -sS --max-time 5 -X POST "$URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json,text/event-stream" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}')
have_tool() {
  printf '%s' "$tools_body" | python3 -c "
import json, sys
names = {t['name'] for t in json.loads(sys.stdin.read())['result']['tools']}
sys.exit(0 if '$1' in names else 1)
"
}

failures=0
call() {
  local name="$1" args="$2"
  local body
  body=$(curl -sS --max-time 10 -X POST "$URL" \
    -H "Content-Type: application/json" \
    -H "Accept: application/json,text/event-stream" \
    -H "Mcp-Session-Id: $SID" \
    -d "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\",\"params\":{\"name\":\"$name\",\"arguments\":$args}}")
  local verdict
  if verdict=$(printf '%s' "$body" | python3 -c '
import json, sys
body = sys.stdin.read()
try:
    r = json.loads(body)
    if "error" in r:
        print("FAIL rpc-error " + json.dumps(r["error"]))
        sys.exit(1)
    res = r.get("result", {})
    if res.get("isError"):
        print("FAIL " + json.dumps(res.get("content", [])))
        sys.exit(1)
    text = res.get("content", [{}])[0].get("text", "")
    summary = json.loads(text) if text else res
    if isinstance(summary, dict):
        keys = [k for k in sorted(summary.keys()) if k != "image_b64"][:6]
        print("ok " + " ".join(keys))
    else:
        print("ok")
except Exception as e:
    print("FAIL parse: " + str(e) + " | " + body[:160])
    sys.exit(1)
'); then
    printf '%-14s %s\n' "$name" "$verdict"
  else
    failures=$((failures+1))
    printf '%-14s %s\n' "$name" "$verdict"
  fi
}

call get_status     '{}'
call pause          '{}'
call mem_read       '{"address":1024,"length":8}'
call mem_write      '{"address":1280,"hex":"deadbeef"}'
call mem_read       '{"address":1280,"length":4}'
call memory_search  '{"hex":"deadbeef","start":1024,"end":1536,"max_results":4}'
call resume         '{}'
call screenshot     '{}'
# run_command must come before send_keys: send_keys leaves text at the
# DOS prompt that the next ParseLine would prefix to its own command.
if have_tool run_command; then
  call run_command  '{"command":"VER"}'
  call run_command  '{"command":"ECHO hello"}'
  call run_command  '{"command":"DIR"}'
else
  echo "run_command    skipped (server build predates the tool)"
fi
call send_key       '{"key":"esc"}'
call send_keys      '{"text":"hi"}'
if have_tool set_speed; then
  call set_speed    '{"multiplier":1.5}'
  call set_speed    '{"multiplier":1.0}'
else
  echo "set_speed      skipped (server build predates the tool)"
fi

curl -sS --max-time 5 -X DELETE "$URL" -H "Mcp-Session-Id: $SID" -o /dev/null

if [ "$failures" -ne 0 ]; then
  echo "FAILED: $failures tool(s) returned isError or unparseable" >&2
  exit 1
fi
echo "all ok"
