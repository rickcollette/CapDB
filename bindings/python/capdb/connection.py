"""CapDB connection and cursor implementation."""

import sqlite3
import threading
import time
import os
from typing import Any, List, Optional, Tuple
from urllib.parse import urlparse, parse_qs

from .errors import (
    OperationalError,
    ProgrammingError,
    IntegrityError,
    DatabaseError,
)


class Connection:
    """DB API 2.0 Connection object for CapDB."""

    def __init__(self, dsn: str, timeout: float = 5.0):
        """
        Initialize CapDB connection.

        Args:
            dsn: Data source name (e.g., "capdb://host:port/db.capdb?token=secret")
            timeout: Connection timeout in seconds
        """
        self.dsn = dsn
        self.timeout = timeout
        self._db: Optional[sqlite3.Connection] = None
        self._mode = self._parse_dsn(dsn)
        self._lock = threading.RLock()
        self._connect()

    def _parse_dsn(self, dsn: str) -> str:
        """Parse DSN and return mode (network or embedded)."""
        if dsn.startswith("capdb://"):
            return "network"
        elif dsn.startswith("file:"):
            return "embedded"
        elif dsn.endswith(".capdb"):
            return "embedded"
        else:
            raise ValueError(f"Invalid DSN format: {dsn}")

    def _connect(self):
        """Establish connection to CapDB."""
        with self._lock:
            if self._mode == "embedded":
                # For embedded mode, use the path directly
                path = self.dsn.replace("file:", "")
                self._db = sqlite3.connect(path, timeout=self.timeout)
            else:
                # For network mode, we need to communicate with capdb-server
                # Using sqlite3 as placeholder - in production would use C FFI
                raise OperationalError(
                    "Network mode requires C FFI bindings (not yet implemented)"
                )

    def cursor(self) -> "Cursor":
        """Create a new cursor."""
        if self._db is None:
            raise OperationalError("Connection is closed")
        return Cursor(self._db)

    def commit(self):
        """Commit the current transaction."""
        with self._lock:
            if self._db is None:
                raise OperationalError("Connection is closed")
            self._db.commit()

    def rollback(self):
        """Rollback the current transaction."""
        with self._lock:
            if self._db is None:
                raise OperationalError("Connection is closed")
            self._db.rollback()

    def close(self):
        """Close the connection."""
        with self._lock:
            if self._db is not None:
                self._db.close()
                self._db = None

    def __enter__(self):
        """Context manager entry."""
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit."""
        if exc_type is not None:
            self.rollback()
        else:
            self.commit()
        self.close()

    def __del__(self):
        """Cleanup on deletion."""
        self.close()


class Cursor:
    """DB API 2.0 Cursor object for CapDB."""

    def __init__(self, db: sqlite3.Connection):
        """Initialize cursor."""
        self._db = db
        self._cursor: Optional[sqlite3.Cursor] = None
        self._arraysize = 1
        self.description: Optional[List] = None
        self.rowcount = -1
        self._row_factory = None

    def execute(
        self, operation: str, parameters: Optional[Tuple] = None
    ) -> "Cursor":
        """
        Execute a SQL statement.

        Args:
            operation: SQL statement
            parameters: Parameters for the statement (tuple or dict)

        Returns:
            self
        """
        if not operation:
            raise ProgrammingError("Empty SQL statement")

        try:
            if parameters is None:
                self._cursor = self._db.execute(operation)
            else:
                self._cursor = self._db.execute(operation, parameters)

            # Update description from cursor
            if self._cursor.description:
                self.description = [
                    (col[0], None, None, None, None, None, None)
                    for col in self._cursor.description
                ]
            else:
                self.description = None

            self.rowcount = self._cursor.rowcount
            return self

        except sqlite3.IntegrityError as e:
            raise IntegrityError(str(e)) from e
        except sqlite3.OperationalError as e:
            raise OperationalError(str(e)) from e
        except sqlite3.ProgrammingError as e:
            raise ProgrammingError(str(e)) from e
        except sqlite3.Error as e:
            raise DatabaseError(str(e)) from e

    def executemany(
        self, operation: str, seq_of_parameters: List[Tuple]
    ) -> "Cursor":
        """Execute a SQL statement with multiple parameter sets."""
        if not operation:
            raise ProgrammingError("Empty SQL statement")

        try:
            for parameters in seq_of_parameters:
                self.execute(operation, parameters)
            return self
        except Exception as e:
            raise DatabaseError(str(e)) from e

    def fetchone(self) -> Optional[Tuple]:
        """Fetch one row from the result set."""
        if self._cursor is None:
            raise ProgrammingError("No active query")
        row = self._cursor.fetchone()
        return row

    def fetchall(self) -> List[Tuple]:
        """Fetch all rows from the result set."""
        if self._cursor is None:
            raise ProgrammingError("No active query")
        return self._cursor.fetchall()

    def fetchmany(self, size: Optional[int] = None) -> List[Tuple]:
        """Fetch specified number of rows from the result set."""
        if self._cursor is None:
            raise ProgrammingError("No active query")
        if size is None:
            size = self._arraysize
        return self._cursor.fetchmany(size)

    def setinputsizes(self, sizes: List[Optional[int]]):
        """DB API 2.0 method - set input sizes (no-op)."""
        pass

    def setoutputsize(self, size: int, column: Optional[int] = None):
        """DB API 2.0 method - set output size (no-op)."""
        pass

    def close(self):
        """Close the cursor."""
        if self._cursor is not None:
            self._cursor.close()
            self._cursor = None

    def __enter__(self):
        """Context manager entry."""
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit."""
        self.close()

    def __iter__(self):
        """Iterate over result rows."""
        if self._cursor is None:
            raise ProgrammingError("No active query")
        return self._cursor


def connect(
    dsn: str,
    timeout: float = 5.0,
    **kwargs
) -> Connection:
    """
    Create a connection to CapDB.

    Args:
        dsn: Data source name:
            - Embedded: "file:path/to/db.capdb" or "path/to/db.capdb"
            - Network: "capdb://host:port/db.capdb?token=secret&insecure=1"
        timeout: Connection timeout in seconds
        **kwargs: Additional connection parameters

    Returns:
        Connection object

    Example:
        >>> conn = connect("file:test.capdb")
        >>> cursor = conn.cursor()
        >>> cursor.execute("CREATE TABLE users (id INTEGER, name TEXT)")
        >>> cursor.execute("INSERT INTO users VALUES (?, ?)", (1, "Alice"))
        >>> conn.commit()
        >>> cursor.execute("SELECT * FROM users")
        >>> print(cursor.fetchall())
        [(1, 'Alice')]
    """
    return Connection(dsn, timeout=timeout, **kwargs)
