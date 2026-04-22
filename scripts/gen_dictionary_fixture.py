#!/usr/bin/env python3
"""
gen_dictionary_fixture.py — dictionary 테이블 fixture 생성기

사전 벤치마크용 (영한) 10 만 행 규모 데이터. ASCII 소문자 영어 key +
한글 VARCHAR value 쌍. 서버 기동 전에 한 번 실행해 data/tables/ 와
data/schema/ 에 배치.

사용:
    python3 scripts/gen_dictionary_fixture.py                  # 10 만 행 기본
    python3 scripts/gen_dictionary_fixture.py --rows 500000   # 50 만
    python3 scripts/gen_dictionary_fixture.py --rows 10000    # 소규모
    python3 scripts/gen_dictionary_fixture.py --data-dir ./data2

출력:
    <data-dir>/schema/dictionary.schema
    <data-dir>/tables/dictionary.csv

CSV 포맷 (기존 storage.c 호환):
    id,english,korean
    1,aaa,한글1
    2,aab,한글2
    ...

id 는 1 부터 시작. english 는 a-z 3~6 글자 랜덤 조합 (중복 없음 보장).
korean 은 짧은 한글 단어 풀에서 랜덤 (중복 허용 — 역방향 조회 시 선형
스캔이 여러 매치를 반환하도록 의도).

결정론적 출력: --seed 옵션으로 고정 (기본 seed=42). 동일 seed 는 동일
CSV 생성.
"""

from __future__ import annotations

import argparse
import os
import random
import string
import sys
from pathlib import Path


KOREAN_WORD_POOL = [
    "사과", "바나나", "체리", "포도", "딸기", "오렌지", "복숭아", "수박",
    "배", "감", "자두", "망고", "파인애플", "키위", "라임", "레몬",
    "멜론", "블루베리", "라즈베리", "크랜베리", "석류",
    "고양이", "강아지", "사자", "호랑이", "코끼리", "원숭이", "토끼", "여우",
    "늑대", "곰", "사슴", "염소", "양", "소", "돼지", "닭",
    "바다", "산", "강", "호수", "숲", "사막", "초원", "섬",
    "도시", "마을", "집", "다리", "길", "공원", "학교", "병원",
    "사랑", "행복", "슬픔", "기쁨", "두려움", "희망", "용기", "평화",
    "시간", "공간", "세상", "사람", "친구", "가족", "마음", "기억",
]


def english_word(n: int) -> str:
    """n -> lowercase ASCII word. 0-base.
    길이 3 (26^3 = 17576) 부족 시 4/5/6 자 확장.
    결정론적: 같은 n 은 같은 word."""
    letters = string.ascii_lowercase
    out = []
    # base-26 encoding — 길이에 따라 prefix 달라짐
    if n < 26**3:                  # 3자: aaa ~ zzz
        length = 3
    elif n < 26**4:
        length = 4
        n -= 26**3
    elif n < 26**5:
        length = 5
        n -= 26**4
    else:
        length = 6
        n -= 26**5
    for _ in range(length):
        out.append(letters[n % 26])
        n //= 26
    return "".join(reversed(out))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rows", type=int, default=100_000,
                    help="row 수 (기본 100,000)")
    ap.add_argument("--data-dir", default="./data",
                    help="data 디렉토리 루트 (기본 ./data)")
    ap.add_argument("--seed", type=int, default=42,
                    help="random seed (기본 42)")
    args = ap.parse_args()

    if args.rows <= 0:
        print("rows must be > 0", file=sys.stderr)
        return 1

    random.seed(args.seed)

    data_dir = Path(args.data_dir).resolve()
    schema_dir = data_dir / "schema"
    tables_dir = data_dir / "tables"
    schema_dir.mkdir(parents=True, exist_ok=True)
    tables_dir.mkdir(parents=True, exist_ok=True)

    schema_path = schema_dir / "dictionary.schema"
    csv_path    = tables_dir / "dictionary.csv"

    # schema format: one column per line as "name,TYPE" (storage.c 호환)
    schema_path.write_text(
        "id,INT\nenglish,VARCHAR\nkorean,VARCHAR\n",
        encoding="utf-8"
    )

    with csv_path.open("w", encoding="utf-8") as f:
        for i in range(args.rows):
            eng = english_word(i)
            kor = random.choice(KOREAN_WORD_POOL)
            f.write(f"{i + 1},{eng},{kor}\n")

    print(f"wrote {args.rows} rows → {csv_path}")
    print(f"schema → {schema_path}")
    print("서버 기동 시 engine_init 이 자동 감지해 Trie 를 rebuild 함.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
