#!/usr/bin/env bash
# run_demo.sh — 발표 데모 부트스트랩 (지용)
#
# 1) make 빌드
# 2) make test (단위 테스트 통과 확인)
# 3) ./sqlparser query.sql --debug (CLI 데모)
# 4) python3 server.py 8000 백그라운드 실행
# 5) 사용자에게 브라우저 URL 안내
#
# 종료: Ctrl+C → 서버 정리 후 종료

set -e
cd "$(dirname "$0")"

PORT="${1:-8000}"

echo "═══════════════════════════════════════════════════════"
echo " MiniSQL — 발표 데모"
echo "═══════════════════════════════════════════════════════"

echo
echo "▶ [1/4] 빌드"
make

echo
echo "▶ [2/4] 단위 테스트"
make test

echo
echo "▶ [3/4] CLI 데모 (--debug)"
echo "─────────────────────────────────────────"
./sqlparser query.sql --debug
echo "─────────────────────────────────────────"

echo
echo "▶ [4/4] HTTP 서버 시작 (포트 $PORT)"
python3 server.py "$PORT" &
SERVER_PID=$!

cleanup() {
    echo
    echo "▶ 정리 중..."
    kill "$SERVER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    echo "▶ 종료. 수고하셨습니다 👋"
}
trap cleanup EXIT INT TERM

sleep 1
echo
echo "═══════════════════════════════════════════════════════"
echo " 브라우저에서 열기:  http://localhost:$PORT"
echo "═══════════════════════════════════════════════════════"
echo
echo " (Ctrl+C 로 종료)"
echo

wait "$SERVER_PID"
