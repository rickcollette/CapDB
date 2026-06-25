//go:build capdb

package capdb

import (
	"context"
	"database/sql"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func startServer(t *testing.T) *sql.DB {
	t.Helper()

	serverBin := os.Getenv("CAPDB_SERVER")
	if serverBin == "" {
		serverBin = filepath.FromSlash("../../build/capdb-server")
	}
	if _, err := os.Stat(serverBin); err != nil {
		t.Skipf("capdb-server not built (%s); run `make capdb`", serverBin)
	}

	dir := t.TempDir()
	const token = "test-token"
	authFile := filepath.Join(dir, "auth.txt")
	if err := os.WriteFile(authFile, []byte(token+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}

	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	l.Close()

	cmd := exec.Command(serverBin,
		"--insecure",
		"--listen", fmt.Sprintf("127.0.0.1:%d", port),
		"--auth-file", authFile,
		"--db-root", dir,
		"--quiet",
	)
	if err := cmd.Start(); err != nil {
		t.Fatalf("start capdb-server: %v", err)
	}
	t.Cleanup(func() {
		_ = cmd.Process.Kill()
		_, _ = cmd.Process.Wait()
	})

	addr := fmt.Sprintf("127.0.0.1:%d", port)
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		c, err := net.DialTimeout("tcp", addr, 200*time.Millisecond)
		if err == nil {
			c.Close()
			break
		}
		time.Sleep(50 * time.Millisecond)
	}

	dsn := fmt.Sprintf("capdb://%s/test.db?token=%s&insecure=1", addr, token)
	db, err := sql.Open("capdb", dsn)
	if err != nil {
		t.Fatal(err)
	}
	db.SetMaxOpenConns(4)
	t.Cleanup(func() { db.Close() })

	for time.Now().Before(deadline) {
		if err = db.Ping(); err == nil {
			return db
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("capdb server never became ready: %v", err)
	return nil
}

func TestConformance(t *testing.T) {
	db := startServer(t)

	if _, err := db.Exec(`CREATE TABLE t(
		id    INTEGER PRIMARY KEY,
		name  TEXT,
		score REAL,
		data  BLOB,
		note  TEXT
	)`); err != nil {
		t.Fatalf("create: %v", err)
	}

	blob := []byte{0x00, 0x01, 0xfe, 0xff}
	if _, err := db.Exec(`INSERT INTO t(id,name,score,data,note) VALUES(?,?,?,?,?)`,
		int64(1), "alice", 9.5, blob, nil); err != nil {
		t.Fatalf("insert: %v", err)
	}
	if _, err := db.Exec(`INSERT INTO t(id,name,score,data,note) VALUES(?,?,?,?,?)`,
		int64(2), "bob's \"quote\"", 3.25, []byte{}, "ok"); err != nil {
		t.Fatalf("insert2: %v", err)
	}

	var (
		name  string
		nm    sql.NullString
		score float64
		data  []byte
	)
	row := db.QueryRow(`SELECT name,score,data,note FROM t WHERE id=?`, int64(1))
	if err := row.Scan(&name, &score, &data, &nm); err != nil {
		t.Fatalf("scan: %v", err)
	}
	if name != "alice" || score != 9.5 || string(data) != string(blob) || nm.Valid {
		t.Fatalf("roundtrip mismatch: name=%q score=%v data=%x note.valid=%v", name, score, data, nm.Valid)
	}

	var bobName string
	if err := db.QueryRow(`SELECT name FROM t WHERE id=?`, int64(2)).Scan(&bobName); err != nil {
		t.Fatalf("scan bob: %v", err)
	}
	if bobName != `bob's "quote"` {
		t.Fatalf("escaping failed: got %q", bobName)
	}

	res, err := db.Exec(`UPDATE t SET score=score+1 WHERE id IN (1,2)`)
	if err != nil {
		t.Fatalf("update: %v", err)
	}
	if n, _ := res.RowsAffected(); n != 2 {
		t.Fatalf("RowsAffected = %d, want 2", n)
	}

	res, err = db.Exec(`UPDATE t SET score=0 WHERE id=?`, int64(999))
	if err != nil {
		t.Fatalf("update nomatch: %v", err)
	}
	if n, _ := res.RowsAffected(); n != 0 {
		t.Fatalf("RowsAffected(no match) = %d, want 0", n)
	}

	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM t`).Scan(&count); err != nil {
		t.Fatalf("count: %v", err)
	}
	if count != 2 {
		t.Fatalf("count = %d, want 2", count)
	}
}

func TestTransactions(t *testing.T) {
	db := startServer(t)
	if _, err := db.Exec(`CREATE TABLE c(n INTEGER)`); err != nil {
		t.Fatal(err)
	}

	tx, err := db.Begin()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(`INSERT INTO c VALUES(?)`, int64(1)); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(`INSERT INTO c VALUES(?)`, int64(2)); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatalf("commit: %v", err)
	}

	tx, err = db.Begin()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(`INSERT INTO c VALUES(?)`, int64(3)); err != nil {
		t.Fatal(err)
	}
	if err := tx.Rollback(); err != nil {
		t.Fatalf("rollback: %v", err)
	}

	var n int
	if err := db.QueryRow(`SELECT COUNT(*) FROM c`).Scan(&n); err != nil {
		t.Fatal(err)
	}
	if n != 2 {
		t.Fatalf("after commit(2) + rollback(1): count = %d, want 2", n)
	}
}

func TestConcurrent(t *testing.T) {
	db := startServer(t)
	if _, err := db.Exec(`CREATE TABLE k(id INTEGER PRIMARY KEY, v TEXT)`); err != nil {
		t.Fatal(err)
	}

	const n = 20
	var wg sync.WaitGroup
	errs := make(chan error, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			if _, err := db.Exec(`INSERT INTO k(id,v) VALUES(?,?)`, int64(i), fmt.Sprintf("v%d", i)); err != nil {
				errs <- err
			}
		}(i)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatalf("concurrent insert: %v", err)
	}

	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM k`).Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != n {
		t.Fatalf("concurrent count = %d, want %d", count, n)
	}
}

func TestContextCancel(t *testing.T) {
	db := startServer(t)
	if _, err := db.Exec(`CREATE TABLE q(n INTEGER)`); err != nil {
		t.Fatal(err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := db.QueryContext(ctx, `SELECT 1`); err == nil {
		t.Fatal("expected error from cancelled context")
	}
	if _, err := db.ExecContext(ctx, `INSERT INTO q VALUES(1)`); err == nil {
		t.Fatal("expected error from cancelled context")
	}

	if _, err := db.Exec(`INSERT INTO q VALUES(2)`); err != nil {
		t.Fatalf("post-cancel exec: %v", err)
	}
}

func TestTimestamps(t *testing.T) {
	db := startServer(t)
	if _, err := db.Exec(`CREATE TABLE ts(id INTEGER, created_at TEXT DEFAULT CURRENT_TIMESTAMP)`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO ts(id) VALUES(1)`); err != nil {
		t.Fatal(err)
	}

	var id int
	var createdAt time.Time
	if err := db.QueryRow(`SELECT id, created_at FROM ts WHERE id=1`).Scan(&id, &createdAt); err != nil {
		t.Fatalf("scan timestamp: %v", err)
	}
	if createdAt.IsZero() {
		t.Fatal("timestamp should not be zero")
	}
}

func TestEmbeddedConformance(t *testing.T) {
	dbPath := filepath.Join(t.TempDir(), "embedded.capdb")
	db, err := sql.Open("capdb-embedded", dbPath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })

	if _, err := db.Exec(`CREATE TABLE t(
		id    INTEGER PRIMARY KEY,
		name  TEXT,
		score REAL,
		data  BLOB,
		note  TEXT
	)`); err != nil {
		t.Fatalf("create embedded: %v", err)
	}

	blob := []byte{0x00, 0x01, 0xfe}
	res, err := db.Exec(`INSERT INTO t(name, score, data, note) VALUES(?,?,?,?)`,
		"alice", 9.5, blob, nil)
	if err != nil {
		t.Fatalf("insert embedded: %v", err)
	}
	if id, _ := res.LastInsertId(); id != 1 {
		t.Fatalf("embedded LastInsertId = %d, want 1", id)
	}

	var (
		id    int64
		name  string
		score float64
		data  []byte
		note  sql.NullString
	)
	if err := db.QueryRow(`SELECT id,name,score,data,note FROM t`).Scan(
		&id, &name, &score, &data, &note,
	); err != nil {
		t.Fatalf("query embedded: %v", err)
	}
	if id != 1 || name != "alice" || score != 9.5 || string(data) != string(blob) || note.Valid {
		t.Fatalf("embedded roundtrip mismatch: id=%d name=%q score=%v data=%x note.valid=%v",
			id, name, score, data, note.Valid)
	}

	tx, err := db.Begin()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(`INSERT INTO t(name) VALUES(?)`, "rolled-back"); err != nil {
		t.Fatal(err)
	}
	if err := tx.Rollback(); err != nil {
		t.Fatal(err)
	}
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM t`).Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != 1 {
		t.Fatalf("embedded rollback count = %d, want 1", count)
	}
}
