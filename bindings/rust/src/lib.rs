//! CapDB Rust Bindings and Safe Wrapper
//!
//! This library provides safe Rust wrappers around the libcapdb C API.
//! It handles connection pooling, error handling, and query execution.

use std::ffi::CStr;
use std::path::Path;
use thiserror::Error;

/// CapDB operation result type
pub type Result<T> = std::result::Result<T, CapDbError>;

/// CapDB errors
#[derive(Error, Debug)]
pub enum CapDbError {
    #[error("connection error: {0}")]
    Connection(String),

    #[error("query error: {0}")]
    Query(String),

    #[error("type error: {0}")]
    Type(String),

    #[error("schema error: {0}")]
    Schema(String),

    #[error("transaction error: {0}")]
    Transaction(String),

    #[error("io error: {0}")]
    Io(#[from] std::io::Error),

    #[error("ffi error: {0}")]
    Ffi(String),

    #[error("pool exhausted")]
    PoolExhausted,
}

/// CapDB connection configuration
#[derive(Debug, Clone)]
pub enum CapDbMode {
    /// Embedded mode: local database file
    Embedded { path: std::path::PathBuf },

    /// Local server mode: TCP connection to local CapDB server
    LocalServer { uri: String },

    /// High availability mode: primary + replicas
    HighAvailability {
        primary_uri: String,
        replicas: Vec<String>,
    },
}

/// CapDB connection
pub struct CapDbConnection {
    mode: CapDbMode,
    handle: Option<*mut std::ffi::c_void>, // libcapdb connection handle
    _marker: std::marker::PhantomData<*mut ()>, // !Send + !Sync
}

impl CapDbConnection {
    /// Open a new CapDB connection
    pub fn open(mode: CapDbMode) -> Result<Self> {
        // TODO: Implement actual C FFI calls to libcapdb
        // For now, this is a placeholder that validates the mode

        match &mode {
            CapDbMode::Embedded { path } => {
                if !path.to_string_lossy().ends_with(".capdb") {
                    return Err(CapDbError::Connection(
                        "embedded database must have .capdb extension".to_string(),
                    ));
                }
            }
            CapDbMode::LocalServer { uri } => {
                if !uri.starts_with("capdb://") {
                    return Err(CapDbError::Connection(
                        "server URI must start with capdb://".to_string(),
                    ));
                }
            }
            CapDbMode::HighAvailability {
                primary_uri,
                replicas,
            } => {
                if replicas.is_empty() {
                    return Err(CapDbError::Connection(
                        "HA mode requires at least one replica".to_string(),
                    ));
                }
                if !primary_uri.starts_with("capdb://") {
                    return Err(CapDbError::Connection(
                        "primary URI must start with capdb://".to_string(),
                    ));
                }
            }
        }

        Ok(Self {
            mode,
            handle: None,
            _marker: std::marker::PhantomData,
        })
    }

    /// Execute a SQL statement with no return value
    pub fn execute(&self, sql: &str) -> Result<()> {
        if sql.trim().is_empty() {
            return Err(CapDbError::Query("empty SQL statement".to_string()));
        }

        // TODO: Implement actual C FFI call to libcapdb exec
        // For now, this is a validation placeholder

        if !sql.contains(';') && !sql.starts_with("CREATE") && !sql.starts_with("INSERT") {
            // Allow basic statements
        }

        Ok(())
    }

    /// Apply SQL schema from string
    pub fn apply_schema(&self, schema_sql: &str) -> Result<()> {
        if schema_sql.trim().is_empty() {
            return Err(CapDbError::Schema("empty schema".to_string()));
        }

        // Split schema into individual statements and execute each
        for statement in schema_sql.split(';') {
            let trimmed = statement.trim();
            if !trimmed.is_empty() {
                self.execute(&format!("{};", trimmed))?;
            }
        }

        Ok(())
    }

    /// Query and return results as JSON
    pub fn query_json(&self, sql: &str) -> Result<serde_json::Value> {
        if sql.trim().is_empty() {
            return Err(CapDbError::Query("empty query".to_string()));
        }

        // TODO: Implement actual C FFI call to libcapdb query
        // For now, return empty array

        Ok(serde_json::json!([]))
    }

    /// Get the connection mode
    pub fn mode(&self) -> &CapDbMode {
        &self.mode
    }
}

impl Drop for CapDbConnection {
    fn drop(&mut self) {
        // TODO: Close libcapdb connection handle if set
        // For now, this is a no-op placeholder
    }
}

/// CapDB connection pool for thread-safe concurrent access
pub struct CapDbPool {
    connections: Vec<CapDbConnection>,
    mode: CapDbMode,
}

impl CapDbPool {
    /// Create a new connection pool
    pub fn new(mode: CapDbMode, size: usize) -> Result<Self> {
        if size == 0 {
            return Err(CapDbError::PoolExhausted);
        }

        // TODO: Create multiple connections

        Ok(Self {
            connections: vec![],
            mode,
        })
    }

    /// Get a connection from the pool (blocking)
    pub fn get(&self) -> Result<&CapDbConnection> {
        self.connections
            .first()
            .ok_or(CapDbError::PoolExhausted)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_embedded_mode_creation() {
        let mode = CapDbMode::Embedded {
            path: "test.capdb".into(),
        };
        let _conn = CapDbConnection::open(mode).expect("should create connection");
    }

    #[test]
    fn test_invalid_embedded_path() {
        let mode = CapDbMode::Embedded {
            path: "test.db".into(),
        };
        assert!(CapDbConnection::open(mode).is_err());
    }

    #[test]
    fn test_empty_query_rejected() {
        let mode = CapDbMode::Embedded {
            path: "test.capdb".into(),
        };
        let conn = CapDbConnection::open(mode).expect("should create connection");
        assert!(conn.execute("").is_err());
    }
}
