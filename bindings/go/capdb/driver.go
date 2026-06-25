//go:build capdb

package capdb

/*
// CapDB client library. Include path and libraries must be supplied at build time
// via CGO_CFLAGS and CGO_LDFLAGS. See ../Makefile and capdb-go-build-env.sh.
#cgo CFLAGS: -DCAPDB_ENABLE_NETWORK=1
#cgo LDFLAGS: -lssl -lcrypto -lpthread
#include <stdlib.h>
#include "capdb_client.h"

// Thin wrapper so Go never has to pass a C function pointer for the
// (unused) row callback.
static int capdb_exec_noload(capdb_conn *p, const char *zSql){
  return capdb_net_exec(p, zSql, 0, 0);
}
*/
import "C"

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"fmt"
	"io"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

// Wire value-type tags returned by capdb_net_column_type.
const (
	valNull  = 0
	valInt   = 1
	valFloat = 2
	valText  = 3
	valBlob  = 4
)

// Network status codes.
const (
	netOK       = 0
	netBusy     = 5
	netAuthFail = 2
)

func init() {
	sql.Register("capdb", Driver{})
}

// Driver is the database/sql driver for CapDB network mode.
type Driver struct{}

func (Driver) Open(dsn string) (driver.Conn, error) {
	c := &conn{}
	curi := C.CString(dsn)
	defer C.free(unsafe.Pointer(curi))
	rc := C.capdb_net_connect(curi, &c.h)
	if rc != netOK || c.h == nil {
		msg := errmsg(c.h)
		if c.h != nil {
			C.capdb_net_close(c.h)
		}
		return nil, fmt.Errorf("capdb: connect failed (rc=%d): %s", int(rc), msg)
	}
	return c, nil
}

// OpenConnector lets database/sql reuse one parsed DSN across the pool.
func (d Driver) OpenConnector(dsn string) (driver.Connector, error) {
	return &connector{dsn: dsn, d: d}, nil
}

type connector struct {
	dsn string
	d   Driver
}

func (c *connector) Connect(ctx context.Context) (driver.Conn, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return c.d.Open(c.dsn)
}

func (c *connector) Driver() driver.Driver { return c.d }

// conn wraps a single capdb_conn. database/sql guarantees a conn is used by at
// most one goroutine at a time; the mutex only guards against the (illegal but
// cheap-to-defend) overlap and makes the race detector happy.
type conn struct {
	mu   sync.Mutex
	h    *C.capdb_conn
	dead atomic.Bool
}

// IsValid reports whether the pool may reuse this connection.
func (c *conn) IsValid() bool { return !c.dead.Load() && c.h != nil }

// watch aborts the current blocking call if ctx is cancelled.
func (c *conn) watch(ctx context.Context) func() {
	if ctx.Done() == nil {
		return func() {}
	}
	stop := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			c.dead.Store(true)
			C.capdb_net_cancel(c.h)
		case <-stop:
		}
	}()
	return func() { close(stop) }
}

func errmsg(h *C.capdb_conn) string {
	if h == nil {
		return "no connection"
	}
	return C.GoString(C.capdb_net_errmsg(h))
}

func (c *conn) err(rc C.int) error {
	if c.h == nil || C.capdb_net_alive(c.h) == 0 {
		c.dead.Store(true)
		return fmt.Errorf("capdb: connection lost (transport error)")
	}
	if int(rc) == netBusy {
		return fmt.Errorf("capdb: %s: %w", errmsg(c.h), driver.ErrBadConn)
	}
	msg := errmsg(c.h)
	if msg == "" {
		switch int(rc) {
		case 19:
			msg = "UNIQUE constraint failed"
		case 787:
			msg = "FOREIGN KEY constraint failed"
		}
	}
	return fmt.Errorf("capdb: %s (rc=%d)", msg, int(rc))
}

func (c *conn) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.h != nil {
		C.capdb_net_close(c.h)
		c.h = nil
	}
	return nil
}

func (c *conn) Prepare(query string) (driver.Stmt, error) {
	return &stmt{c: c, query: query, n: countPlaceholders(query)}, nil
}

func (c *conn) PrepareContext(ctx context.Context, query string) (driver.Stmt, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return c.Prepare(query)
}

func (c *conn) Begin() (driver.Tx, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if err := c.exec("BEGIN"); err != nil {
		return nil, err
	}
	return &capdbTx{c: c}, nil
}

// exec runs SQL statements, splitting multi-statement strings on semicolons.
func (c *conn) exec(sqlText string) error {
	stmts := splitStatements(sqlText)
	for _, stmt := range stmts {
		stmt = strings.TrimSpace(stmt)
		if stmt == "" {
			continue
		}
		csql := C.CString(stmt)
		rc := C.capdb_exec_noload(c.h, csql)
		C.free(unsafe.Pointer(csql))
		if int(rc) != netOK {
			return c.err(rc)
		}
	}
	return nil
}

// ---- Statements ----

type stmt struct {
	c     *conn
	query string
	n     int
}

func (s *stmt) Close() error  { return nil }
func (s *stmt) NumInput() int { return s.n }

func (s *stmt) Exec(args []driver.Value) (driver.Result, error) {
	s.c.mu.Lock()
	defer s.c.mu.Unlock()
	return s.doExec(args)
}

func (s *stmt) ExecContext(ctx context.Context, nargs []driver.NamedValue) (driver.Result, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	s.c.mu.Lock()
	defer s.c.mu.Unlock()
	stop := s.c.watch(ctx)
	defer stop()
	res, err := s.doExec(namedValues(nargs))
	if cerr := ctx.Err(); cerr != nil {
		return nil, cerr
	}
	return res, err
}

func (s *stmt) doExec(args []driver.Value) (driver.Result, error) {
	sqlText, err := substitute(s.query, args)
	if err != nil {
		return nil, err
	}
	if err := s.c.exec(sqlText); err != nil {
		return nil, err
	}
	return result{
		rows:   int64(C.capdb_net_changes(s.c.h)),
		lastID: int64(C.capdb_net_last_insert_rowid(s.c.h)),
	}, nil
}

func (s *stmt) Query(args []driver.Value) (driver.Rows, error) {
	s.c.mu.Lock()
	defer s.c.mu.Unlock()
	return s.doQuery(context.Background(), args)
}

func (s *stmt) QueryContext(ctx context.Context, nargs []driver.NamedValue) (driver.Rows, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	s.c.mu.Lock()
	defer s.c.mu.Unlock()
	stop := s.c.watch(ctx)
	r, err := s.doQuery(ctx, namedValues(nargs))
	if cerr := ctx.Err(); cerr != nil {
		stop()
		return nil, cerr
	}
	if err != nil {
		stop()
		return nil, err
	}
	r.stop = stop
	return r, nil
}

func (s *stmt) doQuery(ctx context.Context, args []driver.Value) (*rows, error) {
	sqlText, err := substitute(s.query, args)
	if err != nil {
		return nil, err
	}
	csql := C.CString(sqlText)
	defer C.free(unsafe.Pointer(csql))
	var st *C.capdb_net_stmt
	if rc := C.capdb_net_prepare(s.c.h, csql, &st); int(rc) != netOK || st == nil {
		return nil, s.c.err(rc)
	}
	ncol := int(C.capdb_net_column_count(st))
	cols := make([]string, ncol)
	for i := range cols {
		cols[i] = fmt.Sprintf("col%d", i)
	}
	return &rows{c: s.c, st: st, cols: cols, ctx: ctx}, nil
}

// ---- Result ----

type result struct {
	rows   int64
	lastID int64
}

func (r result) LastInsertId() (int64, error) { return r.lastID, nil }
func (r result) RowsAffected() (int64, error) { return r.rows, nil }

// ---- Rows ----

type rows struct {
	c    *conn
	st   *C.capdb_net_stmt
	cols []string
	ctx  context.Context
	stop func()
}

func (r *rows) Columns() []string { return r.cols }

func (r *rows) Close() error {
	if r.stop != nil {
		r.stop()
		r.stop = nil
	}
	if r.st != nil {
		C.capdb_net_finalize(r.st)
		r.st = nil
	}
	return nil
}

func (r *rows) Next(dest []driver.Value) error {
	if r.ctx != nil {
		if err := r.ctx.Err(); err != nil {
			return err
		}
	}
	if C.capdb_net_alive(r.c.h) == 0 {
		r.c.dead.Store(true)
		return fmt.Errorf("capdb: connection lost (transport error)")
	}
	rc := C.capdb_net_step(r.st)
	switch int(rc) {
	case 100:
		// fall through
	case 101:
		return io.EOF
	default:
		return r.c.err(rc)
	}
	for i := range dest {
		dest[i] = r.column(i)
	}
	return nil
}

func (r *rows) column(i int) driver.Value {
	ci := C.int(i)
	switch int(C.capdb_net_column_type(r.st, ci)) {
	case valNull:
		return nil
	case valInt:
		return int64(C.capdb_net_column_int64(r.st, ci))
	case valFloat:
		return float64(C.capdb_net_column_double(r.st, ci))
	case valBlob:
		n := C.capdb_net_column_bytes(r.st, ci)
		p := C.capdb_net_column_blob(r.st, ci)
		if p == nil || n == 0 {
			return []byte{}
		}
		return C.GoBytes(p, n)
	default:
		n := C.capdb_net_column_bytes(r.st, ci)
		p := C.capdb_net_column_text(r.st, ci)
		if p == nil {
			return nil
		}
		text := C.GoStringN((*C.char)(unsafe.Pointer(p)), n)
		if tm, err := parseTimestampIfLikely(text); err == nil {
			return tm
		}
		return text
	}
}

// ---- Transactions ----

type capdbTx struct{ c *conn }

func (t *capdbTx) Commit() error {
	t.c.mu.Lock()
	defer t.c.mu.Unlock()
	return t.c.exec("COMMIT")
}

func (t *capdbTx) Rollback() error {
	t.c.mu.Lock()
	defer t.c.mu.Unlock()
	return t.c.exec("ROLLBACK")
}

// ---- Helper functions ----

func namedValues(nv []driver.NamedValue) []driver.Value {
	vs := make([]driver.Value, len(nv))
	for i := range nv {
		vs[i] = nv[i].Value
	}
	return vs
}

func splitStatements(sql string) []string {
	var stmts []string
	var current strings.Builder
	i := 0
	n := len(sql)

	for i < n {
		c := sql[i]
		switch {
		case c == '\'':
			j := i + 1
			for j < n {
				if sql[j] == '\'' {
					if j+1 < n && sql[j+1] == '\'' {
						j += 2
						continue
					}
					j++
					break
				}
				j++
			}
			current.WriteString(sql[i:j])
			i = j
		case c == '"':
			j := i + 1
			for j < n && sql[j] != '"' {
				j++
			}
			if j < n {
				j++
			}
			current.WriteString(sql[i:j])
			i = j
		case c == '[':
			j := i + 1
			for j < n && sql[j] != ']' {
				j++
			}
			if j < n {
				j++
			}
			current.WriteString(sql[i:j])
			i = j
		case c == '-' && i+1 < n && sql[i+1] == '-':
			j := i + 2
			for j < n && sql[j] != '\n' {
				j++
			}
			current.WriteString(sql[i:j])
			i = j
		case c == '/' && i+1 < n && sql[i+1] == '*':
			j := i + 2
			for j+1 < n && !(sql[j] == '*' && sql[j+1] == '/') {
				j++
			}
			if j+1 < n {
				j += 2
			} else {
				j = n
			}
			current.WriteString(sql[i:j])
			i = j
		case c == ';':
			stmts = append(stmts, current.String())
			current.Reset()
			i++
		default:
			current.WriteByte(c)
			i++
		}
	}
	if current.Len() > 0 {
		stmts = append(stmts, current.String())
	}
	return stmts
}

func parseTimestampIfLikely(text string) (time.Time, error) {
	if len(text) < 10 {
		return time.Time{}, fmt.Errorf("too short")
	}
	if text[4] == '-' && text[7] == '-' {
		for _, layout := range []string{
			"2006-01-02 15:04:05.999999999-07:00",
			"2006-01-02 15:04:05.999999999Z07:00",
			"2006-01-02 15:04:05.999999999",
			"2006-01-02 15:04:05",
			"2006-01-02",
		} {
			if tm, err := time.Parse(layout, text); err == nil {
				return tm, nil
			}
		}
	}
	return time.Time{}, fmt.Errorf("not a timestamp")
}
