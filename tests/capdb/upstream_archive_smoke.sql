CREATE TABLE archive_smoke(id INTEGER PRIMARY KEY, name TEXT, payload TEXT);
INSERT INTO archive_smoke(name, payload)
VALUES ('alpha', json_object('n', 1)), ('beta', json_object('n', 2));

WITH ranked AS (
  SELECT name, row_number() OVER (ORDER BY id) AS rn
  FROM archive_smoke
)
SELECT group_concat(name || ':' || rn, ',') FROM ranked;

CREATE INDEX archive_smoke_name ON archive_smoke(name);
SELECT count(*) FROM archive_smoke WHERE name LIKE 'a%';

PRAGMA integrity_check;
