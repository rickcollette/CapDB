import capdb


def test_embedded_roundtrip():
    conn = capdb.connect(":memory:")
    cur = conn.cursor()
    cur.execute("CREATE TABLE t(id INTEGER, name TEXT)")
    cur.execute("INSERT INTO t VALUES(1, 'alice')")
    cur.execute("SELECT id, name FROM t")
    assert cur.fetchall() == [(1, "alice")]
    conn.close()
