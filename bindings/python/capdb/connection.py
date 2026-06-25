"""CapDB DB API 2.0 connection and cursor implementation."""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import threading
from typing import Any, Iterable, List, Optional, Sequence, Tuple

from .errors import (
    DatabaseError,
    IntegrityError,
    NotSupportedError,
    OperationalError,
    ProgrammingError,
)


CAPDB_OK = 0
CAPDB_ROW = 100
CAPDB_DONE = 101

CAPDB_NET_OK = 0
CAPDB_NET_AUTH_FAIL = 2
CAPDB_NET_MISUSE = 3
CAPDB_NET_BUSY = 5

CAPDB_VALUE_NULL = 0
CAPDB_VALUE_INT = 1
CAPDB_VALUE_FLOAT = 2
CAPDB_VALUE_TEXT = 3
CAPDB_VALUE_BLOB = 4

CAPDB_INTEGER = 1
CAPDB_FLOAT = 2
CAPDB_TEXT = 3
CAPDB_BLOB = 4
CAPDB_NULL = 5

CAPDB_OPEN_READWRITE = 0x00000002
CAPDB_OPEN_CREATE = 0x00000004
CAPDB_OPEN_URI = 0x00000040
CAPDB_TRANSIENT = ctypes.c_void_p(-1)


class _CapdbClient:
    """ctypes wrapper around libcapdb."""

    def __init__(self) -> None:
        path = _find_client_library()
        try:
            self.lib = ctypes.CDLL(path)
        except OSError as exc:
            raise OperationalError(f"could not load libcapdb: {exc}") from exc
        self._bind()

    def _bind(self) -> None:
        c_void_pp = ctypes.POINTER(ctypes.c_void_p)

        self.lib.capdb_net_connect.argtypes = [ctypes.c_char_p, c_void_pp]
        self.lib.capdb_net_connect.restype = ctypes.c_int
        self.lib.capdb_net_close.argtypes = [ctypes.c_void_p]
        self.lib.capdb_net_close.restype = ctypes.c_int
        self.lib.capdb_net_exec.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        self.lib.capdb_net_exec.restype = ctypes.c_int
        self.lib.capdb_net_prepare.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            c_void_pp,
        ]
        self.lib.capdb_net_prepare.restype = ctypes.c_int
        self.lib.capdb_net_step.argtypes = [ctypes.c_void_p]
        self.lib.capdb_net_step.restype = ctypes.c_int
        self.lib.capdb_net_finalize.argtypes = [ctypes.c_void_p]
        self.lib.capdb_net_finalize.restype = ctypes.c_int
        self.lib.capdb_net_column_count.argtypes = [ctypes.c_void_p]
        self.lib.capdb_net_column_count.restype = ctypes.c_int
        self.lib.capdb_net_column_type.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_net_column_type.restype = ctypes.c_int
        self.lib.capdb_net_column_int64.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_net_column_int64.restype = ctypes.c_longlong
        self.lib.capdb_net_column_double.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_net_column_double.restype = ctypes.c_double
        self.lib.capdb_net_column_text.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_net_column_text.restype = ctypes.c_void_p
        self.lib.capdb_net_column_blob.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_net_column_blob.restype = ctypes.c_void_p
        self.lib.capdb_net_column_bytes.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_net_column_bytes.restype = ctypes.c_int
        self.lib.capdb_net_errmsg.argtypes = [ctypes.c_void_p]
        self.lib.capdb_net_errmsg.restype = ctypes.c_char_p
        self.lib.capdb_net_changes.argtypes = [ctypes.c_void_p]
        self.lib.capdb_net_changes.restype = ctypes.c_int
        self.lib.capdb_net_last_insert_rowid.argtypes = [ctypes.c_void_p]
        self.lib.capdb_net_last_insert_rowid.restype = ctypes.c_longlong

        self.lib.capdb_open_v2.argtypes = [
            ctypes.c_char_p,
            c_void_pp,
            ctypes.c_int,
            ctypes.c_char_p,
        ]
        self.lib.capdb_open_v2.restype = ctypes.c_int
        self.lib.capdb_close.argtypes = [ctypes.c_void_p]
        self.lib.capdb_close.restype = ctypes.c_int
        self.lib.capdb_exec.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char_p),
        ]
        self.lib.capdb_exec.restype = ctypes.c_int
        self.lib.capdb_prepare_v2.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_int,
            c_void_pp,
            ctypes.POINTER(ctypes.c_char_p),
        ]
        self.lib.capdb_prepare_v2.restype = ctypes.c_int
        self.lib.capdb_step.argtypes = [ctypes.c_void_p]
        self.lib.capdb_step.restype = ctypes.c_int
        self.lib.capdb_finalize.argtypes = [ctypes.c_void_p]
        self.lib.capdb_finalize.restype = ctypes.c_int
        self.lib.capdb_column_count.argtypes = [ctypes.c_void_p]
        self.lib.capdb_column_count.restype = ctypes.c_int
        self.lib.capdb_column_type.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_column_type.restype = ctypes.c_int
        self.lib.capdb_column_int64.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_column_int64.restype = ctypes.c_longlong
        self.lib.capdb_column_double.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_column_double.restype = ctypes.c_double
        self.lib.capdb_column_text.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_column_text.restype = ctypes.c_void_p
        self.lib.capdb_column_blob.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_column_blob.restype = ctypes.c_void_p
        self.lib.capdb_column_bytes.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_column_bytes.restype = ctypes.c_int
        self.lib.capdb_bind_parameter_count.argtypes = [ctypes.c_void_p]
        self.lib.capdb_bind_parameter_count.restype = ctypes.c_int
        self.lib.capdb_bind_null.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.capdb_bind_null.restype = ctypes.c_int
        self.lib.capdb_bind_int64.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_longlong]
        self.lib.capdb_bind_int64.restype = ctypes.c_int
        self.lib.capdb_bind_double.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_double]
        self.lib.capdb_bind_double.restype = ctypes.c_int
        self.lib.capdb_bind_text.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_void_p,
        ]
        self.lib.capdb_bind_text.restype = ctypes.c_int
        self.lib.capdb_bind_blob.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
        ]
        self.lib.capdb_bind_blob.restype = ctypes.c_int
        self.lib.capdb_errmsg.argtypes = [ctypes.c_void_p]
        self.lib.capdb_errmsg.restype = ctypes.c_char_p
        self.lib.capdb_changes.argtypes = [ctypes.c_void_p]
        self.lib.capdb_changes.restype = ctypes.c_int
        self.lib.capdb_last_insert_rowid.argtypes = [ctypes.c_void_p]
        self.lib.capdb_last_insert_rowid.restype = ctypes.c_longlong


_client: Optional[_CapdbClient] = None
_client_lock = threading.Lock()


def _find_client_library() -> str:
    env_path = os.environ.get("CAPDB_LIBRARY")
    if env_path:
        return env_path

    build_dir = os.environ.get("CAPDB_BUILD")
    candidates: List[str] = []
    if build_dir:
        candidates.extend(
            os.path.join(build_dir, name)
            for name in (
                "libcapdb.so",
                "libcapdb.dylib",
                "capdb.dll",
            )
        )
    candidates.extend(
        os.path.abspath(os.path.join(os.getcwd(), name))
        for name in (
            "libcapdb.so",
            "libcapdb.dylib",
            "capdb.dll",
        )
    )
    found = ctypes.util.find_library("capdb")
    if found:
        candidates.append(found)

    for candidate in candidates:
        if os.path.exists(candidate) or os.path.sep not in candidate:
            return candidate
    return "libcapdb.so"


def _get_client() -> _CapdbClient:
    global _client
    with _client_lock:
        if _client is None:
            _client = _CapdbClient()
        return _client


def _errmsg(handle: ctypes.c_void_p) -> str:
    if not handle:
        return "no connection"
    raw = _get_client().lib.capdb_net_errmsg(handle)
    if not raw:
        return "unknown CapDB error"
    return raw.decode("utf-8", "replace")


def _raise_for_rc(rc: int, handle: ctypes.c_void_p) -> None:
    if rc == CAPDB_NET_OK:
        return
    msg = _errmsg(handle)
    if rc == CAPDB_NET_AUTH_FAIL:
        raise OperationalError(f"authentication failed: {msg}")
    if rc == CAPDB_NET_MISUSE:
        raise ProgrammingError(msg)
    if rc == CAPDB_NET_BUSY:
        raise OperationalError(msg)
    if "constraint" in msg.lower():
        raise IntegrityError(msg)
    raise DatabaseError(f"{msg} (rc={rc})")


def _embedded_errmsg(handle: ctypes.c_void_p) -> str:
    if not handle:
        return "no connection"
    raw = _get_client().lib.capdb_errmsg(handle)
    if not raw:
        return "unknown CapDB error"
    return raw.decode("utf-8", "replace")


def _raise_for_embedded_rc(rc: int, handle: ctypes.c_void_p) -> None:
    if rc in (CAPDB_OK, CAPDB_ROW, CAPDB_DONE):
        return
    msg = _embedded_errmsg(handle)
    if "constraint" in msg.lower():
        raise IntegrityError(msg)
    raise DatabaseError(f"{msg} (rc={rc})")


class Connection:
    """DB API 2.0 connection object for CapDB embedded and network sessions."""

    def __init__(self, dsn: str, timeout: float = 5.0):
        if not dsn:
            raise ValueError("DSN cannot be empty")
        if timeout <= 0:
            raise ValueError("Timeout must be positive")
        self.dsn = dsn
        self.timeout = timeout
        self._mode = "network" if dsn.startswith("capdb://") else "embedded"
        self._lock = threading.RLock()
        self._closed = False
        self._handle = ctypes.c_void_p()
        self._transaction_open = False
        self._connect()

    def _connect(self) -> None:
        client = _get_client()
        if self._mode == "network":
            rc = client.lib.capdb_net_connect(
                self.dsn.encode("utf-8"),
                ctypes.byref(self._handle),
            )
            _raise_for_rc(rc, self._handle)
            return

        path = self.dsn[5:] if self.dsn.startswith("file:") else self.dsn
        flags = CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE | CAPDB_OPEN_URI
        rc = client.lib.capdb_open_v2(path.encode("utf-8"), ctypes.byref(self._handle), flags, None)
        _raise_for_embedded_rc(rc, self._handle)

    def _require_open(self) -> ctypes.c_void_p:
        if self._closed or not self._handle:
            raise OperationalError("Connection is closed")
        return self._handle

    def cursor(self) -> "Cursor":
        self._require_open()
        return Cursor(self)

    def commit(self) -> None:
        if self._transaction_open:
            self._exec_direct("COMMIT")
            self._transaction_open = False

    def rollback(self) -> None:
        if self._transaction_open:
            self._exec_direct("ROLLBACK")
            self._transaction_open = False

    def _exec_direct(self, sql: str) -> int:
        with self._lock:
            handle = self._require_open()
            client = _get_client()
            if self._mode == "network":
                rc = client.lib.capdb_net_exec(handle, sql.encode("utf-8"), None, None)
                _raise_for_rc(rc, handle)
                return int(client.lib.capdb_net_changes(handle))
            rc = client.lib.capdb_exec(handle, sql.encode("utf-8"), None, None, None)
            _raise_for_embedded_rc(rc, handle)
            return int(client.lib.capdb_changes(handle))

    def _note_statement(self, sql: str) -> None:
        first = sql.lstrip().split(None, 1)
        if not first:
            return
        op = first[0].upper()
        if op == "BEGIN":
            self._transaction_open = True
        elif op in ("COMMIT", "ROLLBACK", "END"):
            self._transaction_open = False

    def close(self) -> None:
        with self._lock:
            if not self._closed:
                if self._handle:
                    client = _get_client()
                    if self._mode == "network":
                        client.lib.capdb_net_close(self._handle)
                    else:
                        client.lib.capdb_close(self._handle)
                    self._handle = ctypes.c_void_p()
                self._closed = True

    def __enter__(self) -> "Connection":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        if exc_type is not None:
            self.rollback()
        else:
            self.commit()
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


class Cursor:
    """DB API 2.0 cursor object for CapDB network sessions."""

    def __init__(self, connection: Connection):
        self._connection = connection
        self._stmt = ctypes.c_void_p()
        self._arraysize = 1
        self.description: Optional[List[Tuple[Optional[str], None, None, None, None, None, None]]] = None
        self.rowcount = -1
        self._buffered_done = False

    def execute(self, operation: str, parameters: Optional[Sequence[Any]] = None) -> "Cursor":
        if not operation:
            raise ProgrammingError("Empty SQL statement")

        self.close()
        parameters = parameters or ()
        with self._connection._lock:
            handle = self._connection._require_open()
            client = _get_client()
            stmt = ctypes.c_void_p()
            if self._connection._mode == "network":
                sql = _substitute(operation, parameters)
                rc = client.lib.capdb_net_prepare(handle, sql.encode("utf-8"), ctypes.byref(stmt))
                if rc != CAPDB_NET_OK:
                    _raise_for_rc(rc, handle)
            else:
                sql = operation
                rc = client.lib.capdb_prepare_v2(handle, operation.encode("utf-8"), -1, ctypes.byref(stmt), None)
                _raise_for_embedded_rc(rc, handle)
                self._stmt = stmt
                self._bind_embedded(parameters)
            if not self._stmt:
                self._stmt = stmt
            ncol = int(self._column_count())
            self.description = [
                (f"col{i}", None, None, None, None, None, None) for i in range(ncol)
            ] if ncol else None
            if ncol == 0:
                if self._connection._mode == "network":
                    client.lib.capdb_net_finalize(self._stmt)
                    self._stmt = ctypes.c_void_p()
                    rc = client.lib.capdb_net_exec(handle, sql.encode("utf-8"), None, None)
                    _raise_for_rc(rc, handle)
                    self.rowcount = int(client.lib.capdb_net_changes(handle))
                else:
                    rc = client.lib.capdb_step(self._stmt)
                    if rc != CAPDB_DONE:
                        _raise_for_embedded_rc(rc, handle)
                    self.rowcount = int(client.lib.capdb_changes(handle))
            else:
                self.rowcount = -1
            self._connection._note_statement(sql)
        return self

    def executemany(self, operation: str, seq_of_parameters: Iterable[Sequence[Any]]) -> "Cursor":
        if not operation:
            raise ProgrammingError("Empty SQL statement")
        total = 0
        for parameters in seq_of_parameters:
            self.execute(operation, parameters)
            if self.rowcount > 0:
                total += self.rowcount
        self.rowcount = total
        return self

    def fetchone(self) -> Optional[Tuple[Any, ...]]:
        if not self._stmt:
            raise ProgrammingError("No active query")
        if self._buffered_done:
            return None

        with self._connection._lock:
            handle = self._connection._require_open()
            client = _get_client()
            if self._connection._mode == "network":
                rc = int(client.lib.capdb_net_step(self._stmt))
            else:
                rc = int(client.lib.capdb_step(self._stmt))
            if rc == CAPDB_DONE:
                self._buffered_done = True
                return None
            if rc != CAPDB_ROW:
                if self._connection._mode == "network":
                    _raise_for_rc(rc, handle)
                else:
                    _raise_for_embedded_rc(rc, handle)
            return tuple(self._column_value(i) for i in range(self._column_count()))

    def fetchall(self) -> List[Tuple[Any, ...]]:
        rows: List[Tuple[Any, ...]] = []
        while True:
            row = self.fetchone()
            if row is None:
                return rows
            rows.append(row)

    def fetchmany(self, size: Optional[int] = None) -> List[Tuple[Any, ...]]:
        if size is None:
            size = self._arraysize
        rows: List[Tuple[Any, ...]] = []
        for _ in range(size):
            row = self.fetchone()
            if row is None:
                break
            rows.append(row)
        return rows

    def setinputsizes(self, sizes: List[Optional[int]]) -> None:
        pass

    def setoutputsize(self, size: int, column: Optional[int] = None) -> None:
        pass

    def close(self) -> None:
        if self._stmt:
            client = _get_client()
            if self._connection._mode == "network":
                client.lib.capdb_net_finalize(self._stmt)
            else:
                client.lib.capdb_finalize(self._stmt)
            self._stmt = ctypes.c_void_p()
        self._buffered_done = False

    def _drain(self) -> None:
        handle = self._connection._require_open()
        client = _get_client()
        while True:
            rc = int(client.lib.capdb_net_step(self._stmt))
            if rc == CAPDB_DONE:
                return
            if rc != CAPDB_ROW:
                _raise_for_rc(rc, handle)

    def _column_value(self, i: int) -> Any:
        client = _get_client()
        c_index = ctypes.c_int(i)
        if self._connection._mode == "network":
            value_type = int(client.lib.capdb_net_column_type(self._stmt, c_index))
        else:
            value_type = int(client.lib.capdb_column_type(self._stmt, c_index))
        if value_type in (CAPDB_VALUE_NULL, CAPDB_NULL):
            return None
        if value_type in (CAPDB_VALUE_INT, CAPDB_INTEGER):
            if self._connection._mode == "network":
                return int(client.lib.capdb_net_column_int64(self._stmt, c_index))
            return int(client.lib.capdb_column_int64(self._stmt, c_index))
        if value_type in (CAPDB_VALUE_FLOAT, CAPDB_FLOAT):
            if self._connection._mode == "network":
                return float(client.lib.capdb_net_column_double(self._stmt, c_index))
            return float(client.lib.capdb_column_double(self._stmt, c_index))

        if self._connection._mode == "network":
            n = int(client.lib.capdb_net_column_bytes(self._stmt, c_index))
        else:
            n = int(client.lib.capdb_column_bytes(self._stmt, c_index))
        if value_type in (CAPDB_VALUE_BLOB, CAPDB_BLOB):
            if self._connection._mode == "network":
                ptr = client.lib.capdb_net_column_blob(self._stmt, c_index)
            else:
                ptr = client.lib.capdb_column_blob(self._stmt, c_index)
            return bytes(ctypes.string_at(ptr, n)) if ptr and n else b""

        if self._connection._mode == "network":
            ptr = client.lib.capdb_net_column_text(self._stmt, c_index)
        else:
            ptr = client.lib.capdb_column_text(self._stmt, c_index)
        if not ptr:
            return None
        return ctypes.string_at(ptr, n).decode("utf-8", "replace")

    def _column_count(self) -> int:
        client = _get_client()
        if self._connection._mode == "network":
            return int(client.lib.capdb_net_column_count(self._stmt))
        return int(client.lib.capdb_column_count(self._stmt))

    def _bind_embedded(self, parameters: Sequence[Any]) -> None:
        client = _get_client()
        expected = int(client.lib.capdb_bind_parameter_count(self._stmt))
        if len(parameters) != expected:
            raise ProgrammingError(f"expected {expected} parameters, got {len(parameters)}")
        for index, value in enumerate(parameters, start=1):
            if value is None:
                rc = client.lib.capdb_bind_null(self._stmt, index)
            elif isinstance(value, bool):
                rc = client.lib.capdb_bind_int64(self._stmt, index, 1 if value else 0)
            elif isinstance(value, int):
                rc = client.lib.capdb_bind_int64(self._stmt, index, value)
            elif isinstance(value, float):
                rc = client.lib.capdb_bind_double(self._stmt, index, value)
            elif isinstance(value, (bytes, bytearray, memoryview)):
                data = bytes(value)
                buf = ctypes.create_string_buffer(data)
                rc = client.lib.capdb_bind_blob(self._stmt, index, buf, len(data), CAPDB_TRANSIENT)
            elif isinstance(value, str):
                data = value.encode("utf-8")
                rc = client.lib.capdb_bind_text(self._stmt, index, data, len(data), CAPDB_TRANSIENT)
            else:
                raise ProgrammingError(f"unsupported parameter type {type(value).__name__}")
            _raise_for_embedded_rc(int(rc), self._connection._require_open())

    def __enter__(self) -> "Cursor":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        self.close()

    def __iter__(self) -> "Cursor":
        return self

    def __next__(self) -> Tuple[Any, ...]:
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row


def _substitute(operation: str, parameters: Sequence[Any]) -> str:
    if "\x00" in operation:
        raise ProgrammingError("query contains a NUL byte")
    if not parameters:
        return operation

    out: List[str] = []
    argi = 0
    i = 0
    n = len(operation)
    while i < n:
        ch = operation[i]
        if ch == "'":
            j = i + 1
            while j < n:
                if operation[j] == "'":
                    if j + 1 < n and operation[j + 1] == "'":
                        j += 2
                        continue
                    j += 1
                    break
                j += 1
            out.append(operation[i:j])
            i = j
        elif ch == '"':
            j = operation.find('"', i + 1)
            j = n if j < 0 else j + 1
            out.append(operation[i:j])
            i = j
        elif ch == "-" and i + 1 < n and operation[i + 1] == "-":
            j = operation.find("\n", i + 2)
            j = n if j < 0 else j
            out.append(operation[i:j])
            i = j
        elif ch == "/" and i + 1 < n and operation[i + 1] == "*":
            j = operation.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(operation[i:j])
            i = j
        elif ch == "?":
            if i + 1 < n and operation[i + 1].isdigit():
                j = i + 1
                while j < n and operation[j].isdigit():
                    j += 1
                param_index = int(operation[i + 1:j]) - 1
                if param_index < 0 or param_index >= len(parameters):
                    raise ProgrammingError("not enough parameters for placeholders")
                out.append(_literal(parameters[param_index]))
                argi = max(argi, param_index + 1)
                i = j
            else:
                if argi >= len(parameters):
                    raise ProgrammingError("not enough parameters for placeholders")
                out.append(_literal(parameters[argi]))
                argi += 1
                i += 1
        else:
            out.append(ch)
            i += 1

    if argi != len(parameters):
        raise ProgrammingError("too many parameters for placeholders")
    return "".join(out)


def _literal(value: Any) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, (bytes, bytearray, memoryview)):
        return "x'" + bytes(value).hex() + "'"
    if isinstance(value, str):
        if "\x00" in value:
            raise ProgrammingError("string parameter contains a NUL byte")
        return "'" + value.replace("'", "''") + "'"
    raise ProgrammingError(f"unsupported parameter type {type(value).__name__}")


def connect(dsn: str, timeout: float = 5.0, **kwargs: Any) -> Connection:
    """Create a CapDB embedded or network connection."""
    if kwargs:
        unsupported = ", ".join(sorted(kwargs))
        raise NotSupportedError(f"unsupported connection options: {unsupported}")
    return Connection(dsn, timeout)
