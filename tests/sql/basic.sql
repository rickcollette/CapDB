SELECT 1;
SELECT 2+2, 'capdb';
SELECT NULL;
SELECT typeof(1), typeof('x'), typeof(NULL);
CREATE TABLE item(
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  qty INTEGER
);
INSERT INTO item(name, qty) VALUES('alpha', 2);
INSERT INTO item(name, qty) VALUES('beta', 5);
INSERT INTO item(name, qty) VALUES('gamma', NULL);
SELECT id, name, qty FROM item ORDER BY id;
SELECT COUNT(*), SUM(qty) FROM item;
SELECT name FROM item WHERE name LIKE 'a%' OR qty>=5 ORDER BY id;
UPDATE item SET qty = qty + 1 WHERE name = 'alpha';
DELETE FROM item WHERE name = 'gamma';
SELECT name, qty FROM item ORDER BY id;
BEGIN;
INSERT INTO item(name, qty) VALUES('delta', 9);
ROLLBACK;
SELECT COUNT(*) FROM item;
CREATE TABLE tag(item_id INTEGER, tag TEXT);
INSERT INTO tag VALUES(1, 'cold');
INSERT INTO tag VALUES(2, 'hot');
SELECT item.name, tag.tag
  FROM item JOIN tag ON tag.item_id = item.id
  ORDER BY item.id;
CREATE UNIQUE INDEX item_name ON item(name);
SELECT name FROM item WHERE name = 'beta';
