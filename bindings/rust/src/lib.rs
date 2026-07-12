//! CapDB Rust bindings.

use serde_json::{Map, Value};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_double, c_int, c_longlong, c_void};
use std::path::PathBuf;
use std::ptr;
use std::slice;
use thiserror::Error;

pub type Result<T> = std::result::Result<T, CapDbError>;

const CAPDB_OK: c_int = 0;
const CAPDB_ROW: c_int = 100;
const CAPDB_DONE: c_int = 101;
const CAPDB_INTEGER: c_int = 1;
const CAPDB_FLOAT: c_int = 2;
const CAPDB_TEXT: c_int = 3;
const CAPDB_BLOB: c_int = 4;
const CAPDB_NULL: c_int = 5;
const CAPDB_OPEN_READWRITE: c_int = 0x00000002;
const CAPDB_OPEN_CREATE: c_int = 0x00000004;
const CAPDB_OPEN_URI: c_int = 0x00000040;

const CAPDB_NET_OK: c_int = 0;
const CAPDB_VALUE_NULL: c_int = 0;
const CAPDB_VALUE_INT: c_int = 1;
const CAPDB_VALUE_FLOAT: c_int = 2;
const CAPDB_VALUE_TEXT: c_int = 3;
const CAPDB_VALUE_BLOB: c_int = 4;

#[repr(C)]
struct capdb(c_void);
#[repr(C)]
struct capdb_stmt(c_void);
#[repr(C)]
struct capdb_conn(c_void);
#[repr(C)]
struct capdb_net_stmt(c_void);

#[link(name = "capdb")]
extern "C" {
    fn capdb_open_v2(
        filename: *const c_char,
        pp_db: *mut *mut capdb,
        flags: c_int,
        vfs: *const c_char,
    ) -> c_int;
    fn capdb_close(db: *mut capdb) -> c_int;
    fn capdb_exec(
        db: *mut capdb,
        sql: *const c_char,
        cb: *mut c_void,
        arg: *mut c_void,
        errmsg: *mut *mut c_char,
    ) -> c_int;
    fn capdb_prepare_v2(
        db: *mut capdb,
        sql: *const c_char,
        n: c_int,
        pp_stmt: *mut *mut capdb_stmt,
        tail: *mut *const c_char,
    ) -> c_int;
    fn capdb_step(stmt: *mut capdb_stmt) -> c_int;
    fn capdb_finalize(stmt: *mut capdb_stmt) -> c_int;
    fn capdb_column_count(stmt: *mut capdb_stmt) -> c_int;
    fn capdb_column_type(stmt: *mut capdb_stmt, i: c_int) -> c_int;
    fn capdb_column_name(stmt: *mut capdb_stmt, i: c_int) -> *const c_char;
    fn capdb_column_int64(stmt: *mut capdb_stmt, i: c_int) -> c_longlong;
    fn capdb_column_double(stmt: *mut capdb_stmt, i: c_int) -> c_double;
    fn capdb_column_text(stmt: *mut capdb_stmt, i: c_int) -> *const u8;
    fn capdb_column_blob(stmt: *mut capdb_stmt, i: c_int) -> *const c_void;
    fn capdb_column_bytes(stmt: *mut capdb_stmt, i: c_int) -> c_int;
    fn capdb_errmsg(db: *mut capdb) -> *const c_char;
    fn capdb_changes(db: *mut capdb) -> c_int;
    fn capdb_last_insert_rowid(db: *mut capdb) -> c_longlong;
    fn capdb_busy_timeout(db: *mut capdb, ms: c_int) -> c_int;
    fn capdb_bind_null(stmt: *mut capdb_stmt, index: c_int) -> c_int;
    fn capdb_bind_int64(stmt: *mut capdb_stmt, index: c_int, value: c_longlong) -> c_int;
    fn capdb_bind_double(stmt: *mut capdb_stmt, index: c_int, value: c_double) -> c_int;
    fn capdb_bind_text(
        stmt: *mut capdb_stmt,
        index: c_int,
        value: *const c_char,
        length: c_int,
        destructor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) -> c_int;
    fn capdb_bind_blob(
        stmt: *mut capdb_stmt,
        index: c_int,
        value: *const c_void,
        length: c_int,
        destructor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) -> c_int;

    fn capdb_net_connect(uri: *const c_char, pp: *mut *mut capdb_conn) -> c_int;
    fn capdb_net_close(conn: *mut capdb_conn) -> c_int;
    fn capdb_net_exec(
        conn: *mut capdb_conn,
        sql: *const c_char,
        cb: *mut c_void,
        arg: *mut c_void,
    ) -> c_int;
    fn capdb_net_prepare(
        conn: *mut capdb_conn,
        sql: *const c_char,
        pp: *mut *mut capdb_net_stmt,
    ) -> c_int;
    fn capdb_net_step(stmt: *mut capdb_net_stmt) -> c_int;
    fn capdb_net_finalize(stmt: *mut capdb_net_stmt) -> c_int;
    fn capdb_net_column_count(stmt: *mut capdb_net_stmt) -> c_int;
    fn capdb_net_column_type(stmt: *mut capdb_net_stmt, i: c_int) -> c_int;
    fn capdb_net_column_int64(stmt: *mut capdb_net_stmt, i: c_int) -> c_longlong;
    fn capdb_net_column_double(stmt: *mut capdb_net_stmt, i: c_int) -> c_double;
    fn capdb_net_column_text(stmt: *mut capdb_net_stmt, i: c_int) -> *const u8;
    fn capdb_net_column_blob(stmt: *mut capdb_net_stmt, i: c_int) -> *const c_void;
    fn capdb_net_column_bytes(stmt: *mut capdb_net_stmt, i: c_int) -> c_int;
    fn capdb_net_errmsg(conn: *mut capdb_conn) -> *const c_char;
    fn capdb_net_changes(conn: *mut capdb_conn) -> c_int;
    fn capdb_net_last_insert_rowid(conn: *mut capdb_conn) -> c_longlong;
}

#[derive(Error, Debug)]
pub enum CapDbError {
    #[error("connection error: {0}")]
    Connection(String),
    #[error("query error: {0}")]
    Query(String),
    #[error("nul byte in string")]
    Nul(#[from] std::ffi::NulError),
}

#[derive(Debug, Clone)]
pub enum CapDbMode {
    Embedded { path: PathBuf },
    Network { uri: String },
}

enum Handle {
    Embedded(*mut capdb),
    Network(*mut capdb_conn),
}

pub struct CapDbConnection {
    mode: CapDbMode,
    handle: Handle,
}

pub struct ExecuteResult {
    pub rows_affected: i64,
    pub last_insert_rowid: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub enum CapDbValue {
    Null,
    Integer(i64),
    Real(f64),
    Text(String),
    Blob(Vec<u8>),
}

impl From<&str> for CapDbValue {
    fn from(value: &str) -> Self {
        Self::Text(value.to_owned())
    }
}

impl From<String> for CapDbValue {
    fn from(value: String) -> Self {
        Self::Text(value)
    }
}

impl From<i64> for CapDbValue {
    fn from(value: i64) -> Self {
        Self::Integer(value)
    }
}

// CapDB connections may move between threads, but callers must serialize use of
// an individual connection. Deliberately do not implement Sync.
unsafe impl Send for CapDbConnection {}

impl CapDbConnection {
    pub fn open(mode: CapDbMode) -> Result<Self> {
        match &mode {
            CapDbMode::Embedded { path } => {
                let cpath = CString::new(path.to_string_lossy().as_bytes())?;
                let mut db: *mut capdb = ptr::null_mut();
                let flags = CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE | CAPDB_OPEN_URI;
                let rc = unsafe { capdb_open_v2(cpath.as_ptr(), &mut db, flags, ptr::null()) };
                if rc != CAPDB_OK {
                    let msg = embedded_errmsg(db);
                    if !db.is_null() {
                        unsafe { capdb_close(db) };
                    }
                    return Err(CapDbError::Connection(format!("{msg} (rc={rc})")));
                }
                let timeout_rc = unsafe { capdb_busy_timeout(db, 5_000) };
                if timeout_rc != CAPDB_OK {
                    let msg = embedded_errmsg(db);
                    unsafe { capdb_close(db) };
                    return Err(CapDbError::Connection(format!(
                        "failed to configure busy timeout: {msg} (rc={timeout_rc})"
                    )));
                }
                Ok(Self {
                    mode,
                    handle: Handle::Embedded(db),
                })
            }
            CapDbMode::Network { uri } => {
                let curi = CString::new(uri.as_str())?;
                let mut conn: *mut capdb_conn = ptr::null_mut();
                let rc = unsafe { capdb_net_connect(curi.as_ptr(), &mut conn) };
                if rc != CAPDB_NET_OK || conn.is_null() {
                    let msg = network_errmsg(conn);
                    if !conn.is_null() {
                        unsafe { capdb_net_close(conn) };
                    }
                    return Err(CapDbError::Connection(format!("{msg} (rc={rc})")));
                }
                Ok(Self {
                    mode,
                    handle: Handle::Network(conn),
                })
            }
        }
    }

    pub fn execute(&self, sql: &str) -> Result<ExecuteResult> {
        let csql = CString::new(sql)?;
        match self.handle {
            Handle::Embedded(db) => {
                let rc = unsafe {
                    capdb_exec(
                        db,
                        csql.as_ptr(),
                        ptr::null_mut(),
                        ptr::null_mut(),
                        ptr::null_mut(),
                    )
                };
                if rc != CAPDB_OK {
                    return Err(CapDbError::Query(format!(
                        "{} (rc={rc})",
                        embedded_errmsg(db)
                    )));
                }
                Ok(ExecuteResult {
                    rows_affected: unsafe { capdb_changes(db) as i64 },
                    last_insert_rowid: unsafe { capdb_last_insert_rowid(db) as i64 },
                })
            }
            Handle::Network(conn) => {
                let rc = unsafe {
                    capdb_net_exec(conn, csql.as_ptr(), ptr::null_mut(), ptr::null_mut())
                };
                if rc != CAPDB_NET_OK {
                    return Err(CapDbError::Query(format!(
                        "{} (rc={rc})",
                        network_errmsg(conn)
                    )));
                }
                Ok(ExecuteResult {
                    rows_affected: unsafe { capdb_net_changes(conn) as i64 },
                    last_insert_rowid: unsafe { capdb_net_last_insert_rowid(conn) as i64 },
                })
            }
        }
    }

    pub fn apply_schema(&self, schema_sql: &str) -> Result<()> {
        self.execute(schema_sql).map(|_| ())
    }

    pub fn set_busy_timeout(&self, milliseconds: u32) -> Result<()> {
        match self.handle {
            Handle::Embedded(db) => {
                let milliseconds = c_int::try_from(milliseconds)
                    .map_err(|_| CapDbError::Query("busy timeout exceeds i32::MAX".into()))?;
                let rc = unsafe { capdb_busy_timeout(db, milliseconds) };
                if rc == CAPDB_OK {
                    Ok(())
                } else {
                    Err(CapDbError::Query(format!(
                        "{} (rc={rc})",
                        embedded_errmsg(db)
                    )))
                }
            }
            Handle::Network(_) => Err(CapDbError::Query(
                "busy timeout is configured by the CapDB server in network mode".into(),
            )),
        }
    }

    pub fn execute_params(&self, sql: &str, params: &[CapDbValue]) -> Result<ExecuteResult> {
        let Handle::Embedded(db) = self.handle else {
            return Err(CapDbError::Query(
                "parameter binding is not yet supported in network mode".into(),
            ));
        };
        let stmt = prepare_and_bind(db, sql, params)?;
        let rc = unsafe { capdb_step(stmt) };
        let result = if rc == CAPDB_DONE || rc == CAPDB_ROW {
            Ok(ExecuteResult {
                rows_affected: unsafe { capdb_changes(db) as i64 },
                last_insert_rowid: unsafe { capdb_last_insert_rowid(db) as i64 },
            })
        } else {
            Err(CapDbError::Query(format!(
                "{} (rc={rc})",
                embedded_errmsg(db)
            )))
        };
        let finalize_rc = unsafe { capdb_finalize(stmt) };
        if result.is_ok() && finalize_rc != CAPDB_OK {
            return Err(CapDbError::Query(format!(
                "statement finalize failed (rc={finalize_rc})"
            )));
        }
        result
    }

    pub fn query_json_params(&self, sql: &str, params: &[CapDbValue]) -> Result<Value> {
        let Handle::Embedded(db) = self.handle else {
            return Err(CapDbError::Query(
                "parameter binding is not yet supported in network mode".into(),
            ));
        };
        let stmt = prepare_and_bind(db, sql, params)?;
        query_embedded_stmt_json(db, stmt)
    }

    pub fn transaction<T>(&self, operation: impl FnOnce(&Self) -> Result<T>) -> Result<T> {
        self.execute("BEGIN IMMEDIATE")?;
        match operation(self) {
            Ok(value) => {
                if let Err(err) = self.execute("COMMIT") {
                    let _ = self.execute("ROLLBACK");
                    Err(err)
                } else {
                    Ok(value)
                }
            }
            Err(err) => {
                let _ = self.execute("ROLLBACK");
                Err(err)
            }
        }
    }

    pub fn integrity_check(&self) -> Result<bool> {
        let rows = self.query_json("PRAGMA integrity_check")?;
        Ok(rows
            .as_array()
            .and_then(|rows| rows.first())
            .and_then(|row| row.as_object())
            .and_then(|row| row.values().next())
            .and_then(Value::as_str)
            == Some("ok"))
    }

    pub fn checkpoint(&self) -> Result<()> {
        self.query_json("PRAGMA wal_checkpoint(TRUNCATE)")
            .map(|_| ())
    }

    pub fn query_json(&self, sql: &str) -> Result<Value> {
        let csql = CString::new(sql)?;
        match self.handle {
            Handle::Embedded(db) => query_embedded_json(db, csql.as_ptr()),
            Handle::Network(conn) => query_network_json(conn, csql.as_ptr()),
        }
    }

    pub fn mode(&self) -> &CapDbMode {
        &self.mode
    }
}

impl Drop for CapDbConnection {
    fn drop(&mut self) {
        unsafe {
            match self.handle {
                Handle::Embedded(db) if !db.is_null() => {
                    capdb_close(db);
                }
                Handle::Network(conn) if !conn.is_null() => {
                    capdb_net_close(conn);
                }
                _ => {}
            }
        }
    }
}

fn query_embedded_json(db: *mut capdb, sql: *const c_char) -> Result<Value> {
    let mut stmt: *mut capdb_stmt = ptr::null_mut();
    let rc = unsafe { capdb_prepare_v2(db, sql, -1, &mut stmt, ptr::null_mut()) };
    if rc != CAPDB_OK {
        return Err(CapDbError::Query(format!(
            "{} (rc={rc})",
            embedded_errmsg(db)
        )));
    }
    let result = (|| {
        let ncol = unsafe { capdb_column_count(stmt) };
        let names = (0..ncol)
            .map(|i| {
                let p = unsafe { capdb_column_name(stmt, i) };
                if p.is_null() {
                    format!("col{i}")
                } else {
                    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
                }
            })
            .collect::<Vec<_>>();
        let mut rows = Vec::new();
        loop {
            let rc = unsafe { capdb_step(stmt) };
            match rc {
                CAPDB_ROW => {
                    let mut row = Map::new();
                    for i in 0..ncol {
                        row.insert(names[i as usize].clone(), embedded_value(stmt, i));
                    }
                    rows.push(Value::Object(row));
                }
                CAPDB_DONE => break,
                _ => {
                    return Err(CapDbError::Query(format!(
                        "{} (rc={rc})",
                        embedded_errmsg(db)
                    )))
                }
            }
        }
        Ok(Value::Array(rows))
    })();
    unsafe { capdb_finalize(stmt) };
    result
}

fn query_embedded_stmt_json(db: *mut capdb, stmt: *mut capdb_stmt) -> Result<Value> {
    let result = (|| {
        let ncol = unsafe { capdb_column_count(stmt) };
        let names = (0..ncol)
            .map(|i| {
                let p = unsafe { capdb_column_name(stmt, i) };
                if p.is_null() {
                    format!("col{i}")
                } else {
                    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
                }
            })
            .collect::<Vec<_>>();
        let mut rows = Vec::new();
        loop {
            match unsafe { capdb_step(stmt) } {
                CAPDB_ROW => {
                    let mut row = Map::new();
                    for i in 0..ncol {
                        row.insert(names[i as usize].clone(), embedded_value(stmt, i));
                    }
                    rows.push(Value::Object(row));
                }
                CAPDB_DONE => break,
                rc => {
                    return Err(CapDbError::Query(format!(
                        "{} (rc={rc})",
                        embedded_errmsg(db)
                    )))
                }
            }
        }
        Ok(Value::Array(rows))
    })();
    let finalize_rc = unsafe { capdb_finalize(stmt) };
    if result.is_ok() && finalize_rc != CAPDB_OK {
        return Err(CapDbError::Query(format!(
            "statement finalize failed (rc={finalize_rc})"
        )));
    }
    result
}

fn prepare_and_bind(db: *mut capdb, sql: &str, params: &[CapDbValue]) -> Result<*mut capdb_stmt> {
    let sql = CString::new(sql)?;
    let mut stmt = ptr::null_mut();
    let rc = unsafe { capdb_prepare_v2(db, sql.as_ptr(), -1, &mut stmt, ptr::null_mut()) };
    if rc != CAPDB_OK || stmt.is_null() {
        return Err(CapDbError::Query(format!(
            "{} (rc={rc})",
            embedded_errmsg(db)
        )));
    }
    for (offset, value) in params.iter().enumerate() {
        let index = (offset + 1) as c_int;
        let rc = unsafe {
            match value {
                CapDbValue::Null => capdb_bind_null(stmt, index),
                CapDbValue::Integer(value) => capdb_bind_int64(stmt, index, *value),
                CapDbValue::Real(value) => capdb_bind_double(stmt, index, *value),
                CapDbValue::Text(value) => capdb_bind_text(
                    stmt,
                    index,
                    value.as_ptr().cast(),
                    value.len() as c_int,
                    transient_destructor(),
                ),
                CapDbValue::Blob(value) => capdb_bind_blob(
                    stmt,
                    index,
                    value.as_ptr().cast(),
                    value.len() as c_int,
                    transient_destructor(),
                ),
            }
        };
        if rc != CAPDB_OK {
            unsafe { capdb_finalize(stmt) };
            return Err(CapDbError::Query(format!(
                "parameter {index} bind failed: {} (rc={rc})",
                embedded_errmsg(db)
            )));
        }
    }
    Ok(stmt)
}

fn transient_destructor() -> Option<unsafe extern "C" fn(*mut c_void)> {
    // CAPDB_TRANSIENT is the sentinel destructor value -1, matching the C API.
    unsafe { std::mem::transmute::<isize, Option<unsafe extern "C" fn(*mut c_void)>>(-1) }
}

fn query_network_json(conn: *mut capdb_conn, sql: *const c_char) -> Result<Value> {
    let mut stmt: *mut capdb_net_stmt = ptr::null_mut();
    let rc = unsafe { capdb_net_prepare(conn, sql, &mut stmt) };
    if rc != CAPDB_NET_OK || stmt.is_null() {
        return Err(CapDbError::Query(format!(
            "{} (rc={rc})",
            network_errmsg(conn)
        )));
    }
    let result = (|| {
        let ncol = unsafe { capdb_net_column_count(stmt) };
        let mut rows = Vec::new();
        loop {
            let rc = unsafe { capdb_net_step(stmt) };
            match rc {
                CAPDB_ROW => {
                    let mut row = Map::new();
                    for i in 0..ncol {
                        row.insert(format!("col{i}"), network_value(stmt, i));
                    }
                    rows.push(Value::Object(row));
                }
                CAPDB_DONE => break,
                _ => {
                    return Err(CapDbError::Query(format!(
                        "{} (rc={rc})",
                        network_errmsg(conn)
                    )))
                }
            }
        }
        Ok(Value::Array(rows))
    })();
    unsafe { capdb_net_finalize(stmt) };
    result
}

fn embedded_value(stmt: *mut capdb_stmt, i: c_int) -> Value {
    match unsafe { capdb_column_type(stmt, i) } {
        CAPDB_NULL => Value::Null,
        CAPDB_INTEGER => Value::from(unsafe { capdb_column_int64(stmt, i) }),
        CAPDB_FLOAT => Value::from(unsafe { capdb_column_double(stmt, i) }),
        CAPDB_BLOB => {
            let n = unsafe { capdb_column_bytes(stmt, i) };
            let p = unsafe { capdb_column_blob(stmt, i) };
            if p.is_null() || n <= 0 {
                Value::from("")
            } else {
                Value::from(hex(slice_from_ptr(p as *const u8, n)))
            }
        }
        CAPDB_TEXT | _ => text_value(unsafe { capdb_column_text(stmt, i) }, unsafe {
            capdb_column_bytes(stmt, i)
        }),
    }
}

fn network_value(stmt: *mut capdb_net_stmt, i: c_int) -> Value {
    match unsafe { capdb_net_column_type(stmt, i) } {
        CAPDB_VALUE_NULL => Value::Null,
        CAPDB_VALUE_INT => Value::from(unsafe { capdb_net_column_int64(stmt, i) }),
        CAPDB_VALUE_FLOAT => Value::from(unsafe { capdb_net_column_double(stmt, i) }),
        CAPDB_VALUE_BLOB => {
            let n = unsafe { capdb_net_column_bytes(stmt, i) };
            let p = unsafe { capdb_net_column_blob(stmt, i) };
            if p.is_null() || n <= 0 {
                Value::from("")
            } else {
                Value::from(hex(slice_from_ptr(p as *const u8, n)))
            }
        }
        CAPDB_VALUE_TEXT | _ => text_value(unsafe { capdb_net_column_text(stmt, i) }, unsafe {
            capdb_net_column_bytes(stmt, i)
        }),
    }
}

fn text_value(ptr: *const u8, n: c_int) -> Value {
    if ptr.is_null() || n < 0 {
        return Value::Null;
    }
    Value::from(String::from_utf8_lossy(slice_from_ptr(ptr, n)).into_owned())
}

fn slice_from_ptr<'a>(ptr: *const u8, n: c_int) -> &'a [u8] {
    unsafe { slice::from_raw_parts(ptr, n as usize) }
}

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(DIGITS[(b >> 4) as usize] as char);
        out.push(DIGITS[(b & 0x0f) as usize] as char);
    }
    out
}

fn embedded_errmsg(db: *mut capdb) -> String {
    if db.is_null() {
        return "no connection".to_string();
    }
    let p = unsafe { capdb_errmsg(db) };
    if p.is_null() {
        "unknown CapDB error".to_string()
    } else {
        unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
    }
}

fn network_errmsg(conn: *mut capdb_conn) -> String {
    if conn.is_null() {
        return "no connection".to_string();
    }
    let p = unsafe { capdb_net_errmsg(conn) };
    if p.is_null() {
        "unknown CapDB error".to_string()
    } else {
        unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::net::{TcpListener, TcpStream};
    use std::path::Path;
    use std::process::{Command, Stdio};
    use std::thread;
    use std::time::{Duration, Instant};

    #[test]
    fn embedded_roundtrip() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("test.capdb");
        let conn = CapDbConnection::open(CapDbMode::Embedded { path }).unwrap();
        conn.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)")
            .unwrap();
        let res = conn.execute("INSERT INTO t(name) VALUES('alice')").unwrap();
        assert_eq!(res.last_insert_rowid, 1);
        let rows = conn.query_json("SELECT id, name FROM t").unwrap();
        assert_eq!(rows[0]["id"], 1);
        assert_eq!(rows[0]["name"], "alice");
    }

    #[test]
    fn embedded_bound_parameters_and_rollback() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("bound.capdb");
        let conn = CapDbConnection::open(CapDbMode::Embedded { path }).unwrap();
        conn.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
            .unwrap();
        conn.execute_params("INSERT INTO t(name) VALUES(?)", &[CapDbValue::from("a'b")])
            .unwrap();
        let rows = conn
            .query_json_params(
                "SELECT name FROM t WHERE name = ?",
                &[CapDbValue::from("a'b")],
            )
            .unwrap();
        assert_eq!(rows[0]["name"], "a'b");

        let failed: Result<()> = conn.transaction(|tx| {
            tx.execute_params(
                "INSERT INTO t(name) VALUES(?)",
                &[CapDbValue::from("rollback")],
            )?;
            Err(CapDbError::Query("force rollback".into()))
        });
        assert!(failed.is_err());
        let rows = conn
            .query_json("SELECT name FROM t WHERE name = 'rollback'")
            .unwrap();
        assert_eq!(rows.as_array().unwrap().len(), 0);
        assert!(conn.integrity_check().unwrap());
    }

    #[test]
    fn empty_query_rejected_by_engine() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("test.capdb");
        let conn = CapDbConnection::open(CapDbMode::Embedded { path }).unwrap();
        assert!(conn.query_json("SELECT 1").is_ok());
    }

    #[test]
    fn network_roundtrip() {
        let server = std::env::var("CAPDB_SERVER")
            .unwrap_or_else(|_| "../../build/capdb-server".to_string());
        if !Path::new(&server).exists() {
            eprintln!("skipping network_roundtrip: {server} not found");
            return;
        }

        let dir = tempfile::tempdir().unwrap();
        let auth = dir.path().join("auth.txt");
        fs::write(&auth, "test-token\n").unwrap();
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        drop(listener);
        let listen = format!("127.0.0.1:{port}");

        let mut child = Command::new(server)
            .args([
                "--insecure",
                "--listen",
                &listen,
                "--auth-file",
                auth.to_str().unwrap(),
                "--db-root",
                dir.path().to_str().unwrap(),
                "--quiet",
            ])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();

        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline {
            if TcpStream::connect(("127.0.0.1", port)).is_ok() {
                break;
            }
            thread::sleep(Duration::from_millis(50));
        }

        let uri = format!("capdb://127.0.0.1:{port}/test.capdb?token=test-token&insecure=1");
        let conn = CapDbConnection::open(CapDbMode::Network { uri }).unwrap();
        conn.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)")
            .unwrap();
        conn.execute("INSERT INTO t(name) VALUES('alice')").unwrap();
        let rows = conn.query_json("SELECT id, name FROM t").unwrap();
        assert_eq!(rows[0]["col0"], 1);
        assert_eq!(rows[0]["col1"], "alice");

        let _ = child.kill();
        let _ = child.wait();
    }
}
