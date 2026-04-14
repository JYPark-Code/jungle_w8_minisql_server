-- query.sql — 발표 시연용 시나리오 (지용)
--
-- CLAUDE.md 시연 흐름:
--   1) CREATE TABLE
--   2) INSERT 2~3 건
--   3) SELECT *
--   4) WHERE + ORDER BY + LIMIT
--   5) UPDATE
--   6) DELETE → SELECT 로 삭제 확인
--
-- 사용:
--   ./sqlparser query.sql                 # 그냥 실행
--   ./sqlparser query.sql --debug         # AST 트리 같이 보기
--   ./sqlparser query.sql --json          # 파싱 결과 JSON 출력

CREATE TABLE users (id INT, name VARCHAR, age INT, joined DATE);

INSERT INTO users (id, name, age, joined) VALUES (1, 'alice', 25, '2024-01-15');
INSERT INTO users (id, name, age, joined) VALUES (2, 'bob',   31, '2024-03-02');
INSERT INTO users (id, name, age, joined) VALUES (3, 'carol', 28, '2024-06-20');

SELECT * FROM users;

SELECT id, name FROM users WHERE age > 26 ORDER BY age DESC LIMIT 2;

UPDATE users SET age = 26 WHERE name = 'alice';

DELETE FROM users WHERE id = 2;

SELECT * FROM users;
