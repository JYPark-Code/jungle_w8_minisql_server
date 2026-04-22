#!/usr/bin/env python3
"""server.py — MiniSQL HTTP 중계 서버 (지용)
============================================================================

▣ 이 파일이 하는 일
   브라우저 (index.html) 와 C 로 만든 sqlparser 바이너리 사이를 연결해주는
   "다리" 역할을 한다.

   브라우저는 SQL 글자만 알고, sqlparser 는 파일을 입력으로 받는 CLI 다.
   이 둘이 서로 직접 못 만나니까, 가운데에 Python 서버를 둬서:
     1) 브라우저가 보낸 SQL 을 받아서
     2) 임시 파일로 저장하고
     3) ./sqlparser tempfile --json 명령을 실행해서
     4) 결과 JSON 을 브라우저에 돌려준다.

▣ 의존성
   파이썬 표준 라이브러리만 사용 (pip install 필요 없음).
   - http.server : 작은 HTTP 서버
   - subprocess  : 외부 프로그램 (sqlparser) 실행
   - tempfile    : 임시 파일 생성/삭제

▣ 엔드포인트
   GET  /          → index.html 페이지 보내기
   GET  /health    → 서버 살아있는지 + 바이너리 있는지 확인
   POST /query     → {"sql": "..."} 받아서 파싱 결과 JSON 반환

▣ 사용
   python3 server.py            # 8000 포트
   python3 server.py 9000       # 다른 포트 지정

▣ 보안 주의
   이 서버는 localhost / 데모 용. 인터넷에 공개하면 안 됨.
   외부 입력을 그대로 sqlparser 에 넘기는 구조라 신뢰할 수 없는 환경에서
   돌리면 위험.
============================================================================
"""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

# 이 파일이 있는 디렉토리 = 프로젝트 루트
ROOT = Path(__file__).resolve().parent

# 같은 디렉토리에 있는 sqlparser 실행파일과 HTML 페이지
SQLPARSER_BIN = ROOT / "sqlparser"
INDEX_HTML = ROOT / "index.html"


def run_sqlparser(sql: str) -> dict:
    """SQL 문자열을 받아서 sqlparser 실행 → 결과 dict 반환.

    동작 순서:
      1) sqlparser 바이너리가 있는지 확인
      2) 임시 .sql 파일 만들어서 SQL 쓰기
      3) subprocess 로 ./sqlparser tempfile --json 실행 (5초 timeout)
      4) 임시 파일 삭제
      5) stdout 한 줄씩 JSON 파싱해서 statements 배열로
      6) statements + stderr + returncode 반환

    timeout 으로 무한 루프나 너무 큰 입력 방어.
    """
    # 1) 바이너리 존재 확인
    if not SQLPARSER_BIN.exists():
        return {
            "error": f"sqlparser binary not found at {SQLPARSER_BIN}. "
                     f"먼저 'make' 로 빌드하세요.",
            "statements": [],
            "stderr": "",
        }

    # 2) 임시 SQL 파일 생성. delete=False 인 이유: with 블록을 빠져나가도
    #    자동 삭제되지 않게 하고, finally 에서 직접 삭제 (Windows 호환).
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".sql", delete=False, encoding="utf-8"
    ) as tf:
        tf.write(sql)
        tmp_path = tf.name

    try:
        # 3) sqlparser 실행. capture_output 으로 stdout/stderr 모두 잡고,
        #    text=True 로 자동 디코딩, timeout 5초.
        proc = subprocess.run(
            [str(SQLPARSER_BIN), tmp_path, "--json"],
            capture_output=True,
            text=True,
            timeout=5,
        )
    except subprocess.TimeoutExpired:
        return {"error": "sqlparser timeout (5s)", "statements": [], "stderr": ""}
    finally:
        # 4) 성공/실패 무관 임시 파일 정리
        try:
            os.unlink(tmp_path)
        except OSError:
            pass

    # 5) stdout 분리 — JSON 줄과 SELECT 결과 표 줄이 섞여 들어온다.
    #
    #    sqlparser --json 출력 형태:
    #      {"type":"CREATE",...}              ← JSON 줄  (statement 시작)
    #      {"type":"INSERT",...}              ← JSON 줄
    #      {"type":"SELECT",...}              ← JSON 줄
    #      id | name                          ← 직전 SELECT 결과 표 시작
    #      1 | alice
    #      2 | bob
    #      (2 rows)
    #      {"type":"DELETE",...}              ← 다음 JSON 줄
    #
    #    한 줄씩 읽으면서:
    #      - JSON parse 성공 → statement 추가
    #      - JSON parse 실패 → 직전 statement 의 _result 에 누적
    statements = []
    pending_result_lines = []

    def flush_result():
        """누적된 결과 줄을 직전 statement 에 attach."""
        if statements and pending_result_lines:
            # 빈 줄 끝부분 제거
            text = "\n".join(pending_result_lines).rstrip()
            if text:
                statements[-1]["_result"] = text

    for line in proc.stdout.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        try:
            parsed = json.loads(stripped)
        except json.JSONDecodeError:
            parsed = None

        # statement 로 인정하는 조건: dict 이고 "type" 키가 있어야 함.
        # 단독 숫자 (예: COUNT(*) 결과 "2") 도 json.loads 는 int 로 성공하지만
        # statement 가 아니라 결과 줄이므로 제외한다.
        is_statement = isinstance(parsed, dict) and "type" in parsed

        if is_statement:
            # 새 statement 시작 — 직전 결과를 먼저 flush
            flush_result()
            pending_result_lines = []
            statements.append(parsed)
        else:
            # 결과 표의 한 줄
            pending_result_lines.append(line)

    # 마지막 statement 의 결과 flush
    flush_result()

    # 6) 결과 묶어서 반환
    return {
        "statements": statements,
        "stderr": proc.stderr,
        "returncode": proc.returncode,
    }


class Handler(BaseHTTPRequestHandler):
    """HTTP 요청 한 건을 처리하는 핸들러 클래스.

    do_GET / do_POST / do_OPTIONS 메서드를 정의하면 표준 라이브러리가
    각각의 HTTP 메서드 요청을 자동으로 이 메서드로 라우팅해준다.
    """

    # ─── 응답 보내기 헬퍼들 ────────────────────────────────────

    def _send(self, status: int, body: bytes, content_type: str):
        """공통 응답 송신. 상태 코드 + 헤더 + 본문."""
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        # CORS: 다른 도메인 (예: file://) 에서도 호출 가능하게 허용.
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, status: int, payload: dict):
        """dict 를 JSON 으로 직렬화해 응답."""
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8")

    # ─── 핸들러 메서드들 ──────────────────────────────────────

    def do_OPTIONS(self):
        """CORS preflight 요청 응답.
        브라우저가 POST 보내기 전에 "이 도메인에서 보내도 돼?" 물어볼 때 사용."""
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        """GET 요청 처리."""
        # 루트 경로 → index.html 페이지 서빙
        if self.path in ("/", "/index.html"):
            if not INDEX_HTML.exists():
                self._send_json(404, {"error": "index.html not found"})
                return
            body = INDEX_HTML.read_bytes()
            self._send(200, body, "text/html; charset=utf-8")
            return

        # 헬스체크: 서버가 살아있고 바이너리가 있는지 확인
        if self.path == "/health":
            self._send_json(200, {"ok": True, "binary_exists": SQLPARSER_BIN.exists()})
            return

        # 그 외 경로는 404
        self._send_json(404, {"error": "not found"})

    def do_POST(self):
        """POST 요청 처리. 현재는 /query 만 지원."""
        if self.path != "/query":
            self._send_json(404, {"error": "not found"})
            return

        # 요청 본문 길이 읽기
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            self._send_json(400, {"error": "empty body"})
            return

        # 본문을 그만큼 읽어서 JSON 으로 파싱
        raw = self.rfile.read(length)
        try:
            data = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": f"invalid JSON: {e}"})
            return

        # sql 필드 추출
        sql = data.get("sql", "").strip()
        if not sql:
            self._send_json(400, {"error": "missing 'sql' field"})
            return

        # sqlparser 실행 후 결과 응답
        result = run_sqlparser(sql)
        self._send_json(200, result)

    def log_message(self, fmt, *args):
        """기본 로그 포맷을 우리 스타일로 덮어쓰기 (stderr 로 출력)."""
        sys.stderr.write(f"[server] {self.address_string()} {fmt % args}\n")


def main():
    """프로그램 진입점. 포트 정해서 서버 시작."""
    # 명령줄 인자가 있으면 그걸 포트로, 없으면 8000.
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000

    # ThreadingHTTPServer: 여러 요청을 동시에 처리할 수 있는 HTTP 서버.
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)

    print(f"[server] MiniSQL viewer listening on http://0.0.0.0:{port}")
    print(f"[server] sqlparser binary: {SQLPARSER_BIN} "
          f"({'found' if SQLPARSER_BIN.exists() else 'NOT FOUND — run make first'})")

    try:
        server.serve_forever()   # 영원히 요청 대기
    except KeyboardInterrupt:
        # Ctrl+C 로 종료하면 인사 한 줄.
        print("\n[server] bye")


if __name__ == "__main__":
    # 이 파일이 직접 실행될 때만 main() 호출 (import 시에는 안 함).
    main()
