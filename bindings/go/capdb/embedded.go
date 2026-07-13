//go:build capdb

package capdb

/*
#cgo CFLAGS: -DCAPDB_ENABLE_NETWORK=1
#cgo LDFLAGS: -lssl -lcrypto -lpthread
#include <stdlib.h>
#include "capdb.h"

static int capdb_exec_nocb(capdb *db, const char *zSql){
  return capdb_exec(db, zSql, 0, 0, 0);
}

static int capdb_bind_text_copy(capdb_stmt *s, int i, const char *z, int n){
  return capdb_bind_text(s, i, z, n, CAPDB_TRANSIENT);
}

static int capdb_bind_blob_copy(capdb_stmt *s, int i, const void *z, int n){
  return capdb_bind_blob(s, i, z, n, CAPDB_TRANSIENT);
}
*/
import "C"

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"fmt"
	"io"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"
)

const (
	capdbOK      = 0
	capdbRow     = 100
	capdbDone    = 101
	capdbInteger = 1
	capdbFloat   = 2
	capdbText    = 3
	capdbBlob    = 4
	capdbNull    = 5
)

func init() {
	sql.Register("capdb-embedded", EmbeddedDriver{})
}

// EmbeddedDriver is the database/sql driver for local CapDB files.
type EmbeddedDriver struct{}

func (EmbeddedDriver) Open(dsn string) (driver.Conn, error) {
	path, options, err := parseEmbeddedDSN(dsn)
	if err != nil {
		return nil, err
	}
	cdsn := C.CString(path)
	defer C.free(unsafe.Pointer(cdsn))
	c := &embeddedConn{beginSQL: options.beginSQL}
	flags := C.int(C.CAPDB_OPEN_READWRITE | C.CAPDB_OPEN_CREATE | C.CAPDB_OPEN_URI)
	rc := C.capdb_open_v2(cdsn, &c.db, flags, nil)
	if int(rc) != capdbOK {
		msg := embeddedErrmsg(c.db)
		if c.db != nil {
			C.capdb_close(c.db)
		}
		return nil, fmt.Errorf("capdb: open failed (rc=%d): %s", int(rc), msg)
	}
	if options.busyTimeoutMS >= 0 {
		if rc := C.capdb_busy_timeout(c.db, C.int(options.busyTimeoutMS)); int(rc) != capdbOK {
			msg := embeddedErrmsg(c.db)
			C.capdb_close(c.db)
			return nil, fmt.Errorf("capdb: setting busy timeout failed (rc=%d): %s", int(rc), msg)
		}
	}
	for _, pragma := range options.pragmas {
		if err := c.execLocked(pragma); err != nil {
			C.capdb_close(c.db)
			return nil, fmt.Errorf("capdb: applying embedded DSN option: %w", err)
		}
	}
	return c, nil
}

type embeddedOptions struct {
	beginSQL      string
	busyTimeoutMS int
	pragmas       []string
}

func parseEmbeddedDSN(dsn string) (string, embeddedOptions, error) {
	options := embeddedOptions{beginSQL: "BEGIN", busyTimeoutMS: -1}
	if dsn == "" {
		return "", options, fmt.Errorf("capdb: DSN cannot be empty")
	}
	if strings.HasPrefix(dsn, "file:") {
		dsn = strings.TrimPrefix(dsn, "file:")
	}
	path, rawQuery, _ := strings.Cut(dsn, "?")
	if path == "" {
		return "", options, fmt.Errorf("capdb: DSN path cannot be empty")
	}
	query, err := url.ParseQuery(rawQuery)
	if err != nil {
		return "", options, fmt.Errorf("capdb: invalid embedded DSN options: %w", err)
	}
	for key := range query {
		switch key {
		case "_busy_timeout", "_foreign_keys", "_loc", "_sync", "_txlock":
		default:
			return "", options, fmt.Errorf("capdb: unsupported embedded DSN option %q", key)
		}
	}
	if value := query.Get("_busy_timeout"); value != "" {
		ms, err := strconv.Atoi(value)
		if err != nil || ms < 0 {
			return "", options, fmt.Errorf("capdb: invalid _busy_timeout %q", value)
		}
		options.busyTimeoutMS = ms
	}
	if value := query.Get("_foreign_keys"); value != "" {
		enabled, err := strconv.ParseBool(value)
		if err != nil {
			return "", options, fmt.Errorf("capdb: invalid _foreign_keys %q", value)
		}
		if enabled {
			options.pragmas = append(options.pragmas, "PRAGMA foreign_keys=ON")
		} else {
			options.pragmas = append(options.pragmas, "PRAGMA foreign_keys=OFF")
		}
	}
	if value := query.Get("_sync"); value != "" {
		value = strings.ToUpper(value)
		switch value {
		case "0":
			value = "OFF"
		case "1":
			value = "NORMAL"
		case "2":
			value = "FULL"
		case "3":
			value = "EXTRA"
		case "OFF", "NORMAL", "FULL", "EXTRA":
		default:
			return "", options, fmt.Errorf("capdb: invalid _sync %q", value)
		}
		options.pragmas = append(options.pragmas, "PRAGMA synchronous="+value)
	}
	if value := query.Get("_txlock"); value != "" {
		switch strings.ToLower(value) {
		case "deferred":
			options.beginSQL = "BEGIN DEFERRED"
		case "immediate":
			options.beginSQL = "BEGIN IMMEDIATE"
		case "exclusive":
			options.beginSQL = "BEGIN EXCLUSIVE"
		default:
			return "", options, fmt.Errorf("capdb: invalid _txlock %q", value)
		}
	}
	if value := query.Get("_loc"); value != "" && value != "auto" {
		return "", options, fmt.Errorf("capdb: unsupported _loc %q (only auto is supported)", value)
	}
	return path, options, nil
}

type embeddedConn struct {
	mu       sync.Mutex
	db       *C.capdb
	beginSQL string
}

func embeddedErrmsg(db *C.capdb) string {
	if db == nil {
		return "no connection"
	}
	return C.GoString(C.capdb_errmsg(db))
}

func (c *embeddedConn) err(rc C.int) error {
	return fmt.Errorf("capdb: %s (rc=%d)", embeddedErrmsg(c.db), int(rc))
}

func (c *embeddedConn) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.db != nil {
		rc := C.capdb_close(c.db)
		c.db = nil
		if int(rc) != capdbOK {
			return fmt.Errorf("capdb: close failed (rc=%d)", int(rc))
		}
	}
	return nil
}

func (c *embeddedConn) Prepare(query string) (driver.Stmt, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.db == nil {
		return nil, fmt.Errorf("capdb: connection closed")
	}
	csql := C.CString(query)
	defer C.free(unsafe.Pointer(csql))
	var st *C.capdb_stmt
	rc := C.capdb_prepare_v2(c.db, csql, -1, &st, nil)
	if int(rc) != capdbOK {
		return nil, c.err(rc)
	}
	return &embeddedStmt{c: c, st: st, query: query, n: int(C.capdb_bind_parameter_count(st))}, nil
}

func (c *embeddedConn) PrepareContext(ctx context.Context, query string) (driver.Stmt, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return c.Prepare(query)
}

func (c *embeddedConn) Begin() (driver.Tx, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if err := c.execLocked(c.beginSQL); err != nil {
		return nil, err
	}
	return &embeddedTx{c: c}, nil
}

func (c *embeddedConn) execLocked(sqlText string) error {
	csql := C.CString(sqlText)
	defer C.free(unsafe.Pointer(csql))
	rc := C.capdb_exec_nocb(c.db, csql)
	if int(rc) != capdbOK {
		return c.err(rc)
	}
	return nil
}

type embeddedStmt struct {
	c     *embeddedConn
	st    *C.capdb_stmt
	query string
	n     int
}

func (s *embeddedStmt) Close() error {
	s.c.mu.Lock()
	defer s.c.mu.Unlock()
	if s.st != nil {
		rc := C.capdb_finalize(s.st)
		s.st = nil
		if int(rc) != capdbOK {
			return s.c.err(rc)
		}
	}
	return nil
}

func (s *embeddedStmt) NumInput() int { return s.n }

func (s *embeddedStmt) Exec(args []driver.Value) (driver.Result, error) {
	s.c.mu.Lock()
	defer s.c.mu.Unlock()
	if err := s.bind(args); err != nil {
		return nil, err
	}
	rc := C.capdb_step(s.st)
	if int(rc) != capdbDone {
		err := s.c.err(rc)
		s.reset()
		return nil, err
	}
	res := result{
		rows:   int64(C.capdb_changes(s.c.db)),
		lastID: int64(C.capdb_last_insert_rowid(s.c.db)),
	}
	if err := s.reset(); err != nil {
		return nil, err
	}
	return res, nil
}

func (s *embeddedStmt) ExecContext(ctx context.Context, nargs []driver.NamedValue) (driver.Result, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return s.Exec(namedValues(nargs))
}

func (s *embeddedStmt) Query(args []driver.Value) (driver.Rows, error) {
	s.c.mu.Lock()
	defer s.c.mu.Unlock()
	if err := s.bind(args); err != nil {
		return nil, err
	}
	ncol := int(C.capdb_column_count(s.st))
	cols := make([]string, ncol)
	for i := range cols {
		name := C.capdb_column_name(s.st, C.int(i))
		if name != nil {
			cols[i] = C.GoString(name)
		} else {
			cols[i] = fmt.Sprintf("col%d", i)
		}
	}
	return &embeddedRows{s: s, cols: cols}, nil
}

func (s *embeddedStmt) QueryContext(ctx context.Context, nargs []driver.NamedValue) (driver.Rows, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return s.Query(namedValues(nargs))
}

func (s *embeddedStmt) bind(args []driver.Value) error {
	if len(args) != s.n {
		return fmt.Errorf("capdb: expected %d arguments, got %d", s.n, len(args))
	}
	for i, v := range args {
		idx := C.int(i + 1)
		var rc C.int
		switch t := v.(type) {
		case nil:
			rc = C.capdb_bind_null(s.st, idx)
		case int64:
			rc = C.capdb_bind_int64(s.st, idx, C.capdb_int64(t))
		case float64:
			rc = C.capdb_bind_double(s.st, idx, C.double(t))
		case bool:
			if t {
				rc = C.capdb_bind_int64(s.st, idx, 1)
			} else {
				rc = C.capdb_bind_int64(s.st, idx, 0)
			}
		case []byte:
			if len(t) == 0 {
				rc = C.capdb_bind_blob_copy(s.st, idx, nil, 0)
			} else {
				rc = C.capdb_bind_blob_copy(s.st, idx, unsafe.Pointer(&t[0]), C.int(len(t)))
			}
		case string:
			cs := C.CString(t)
			rc = C.capdb_bind_text_copy(s.st, idx, cs, C.int(len(t)))
			C.free(unsafe.Pointer(cs))
		case time.Time:
			cs := C.CString(t.Format(timeLayout))
			rc = C.capdb_bind_text_copy(s.st, idx, cs, C.int(len(t.Format(timeLayout))))
			C.free(unsafe.Pointer(cs))
		default:
			return fmt.Errorf("capdb: unsupported argument type %T", v)
		}
		if int(rc) != capdbOK {
			return s.c.err(rc)
		}
	}
	return nil
}

func (s *embeddedStmt) reset() error {
	rc := C.capdb_reset(s.st)
	C.capdb_clear_bindings(s.st)
	if int(rc) != capdbOK {
		return s.c.err(rc)
	}
	return nil
}

type embeddedRows struct {
	s    *embeddedStmt
	cols []string
	done bool
}

func (r *embeddedRows) Columns() []string { return r.cols }

func (r *embeddedRows) Close() error {
	r.s.c.mu.Lock()
	defer r.s.c.mu.Unlock()
	if r.done {
		return nil
	}
	r.done = true
	return r.s.reset()
}

func (r *embeddedRows) Next(dest []driver.Value) error {
	r.s.c.mu.Lock()
	defer r.s.c.mu.Unlock()
	if r.done {
		return io.EOF
	}
	rc := C.capdb_step(r.s.st)
	switch int(rc) {
	case capdbRow:
	case capdbDone:
		r.done = true
		if err := r.s.reset(); err != nil {
			return err
		}
		return io.EOF
	default:
		err := r.s.c.err(rc)
		r.s.reset()
		return err
	}
	for i := range dest {
		dest[i] = embeddedColumn(r.s.st, i)
	}
	return nil
}

func embeddedColumn(st *C.capdb_stmt, i int) driver.Value {
	ci := C.int(i)
	switch int(C.capdb_column_type(st, ci)) {
	case capdbNull:
		return nil
	case capdbInteger:
		return int64(C.capdb_column_int64(st, ci))
	case capdbFloat:
		return float64(C.capdb_column_double(st, ci))
	case capdbBlob:
		n := C.capdb_column_bytes(st, ci)
		p := C.capdb_column_blob(st, ci)
		if p == nil || n == 0 {
			return []byte{}
		}
		return C.GoBytes(p, n)
	default:
		n := C.capdb_column_bytes(st, ci)
		p := C.capdb_column_text(st, ci)
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

type embeddedTx struct{ c *embeddedConn }

func (t *embeddedTx) Commit() error {
	t.c.mu.Lock()
	defer t.c.mu.Unlock()
	return t.c.execLocked("COMMIT")
}

func (t *embeddedTx) Rollback() error {
	t.c.mu.Lock()
	defer t.c.mu.Unlock()
	return t.c.execLocked("ROLLBACK")
}
