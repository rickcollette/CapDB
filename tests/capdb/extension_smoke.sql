CREATE VIRTUAL TABLE f USING fts5(x);
INSERT INTO f VALUES('hello world');
SELECT count(*) FROM f WHERE f MATCH 'hello';
SELECT ceil(1.2);
SELECT percentile_cont(v,0.5) FROM (SELECT 1 AS v UNION ALL SELECT 3);
