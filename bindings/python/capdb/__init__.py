"""
CapDB Python Driver

A DB API 2.0-compliant driver for CapDB embedded and network modes.

Example:
    >>> import capdb
    >>> conn = capdb.connect("capdb://localhost:5432/db.capdb?token=secret")
    >>> cursor = conn.cursor()
    >>> cursor.execute("SELECT * FROM users")
    >>> rows = cursor.fetchall()
"""

from .connection import connect
from .errors import (
    Error,
    InterfaceError,
    DatabaseError,
    DataError,
    OperationalError,
    IntegrityError,
    InternalError,
    ProgrammingError,
    NotSupportedError,
)

__version__ = "0.1.0"
__all__ = [
    "connect",
    "Error",
    "InterfaceError",
    "DatabaseError",
    "DataError",
    "OperationalError",
    "IntegrityError",
    "InternalError",
    "ProgrammingError",
    "NotSupportedError",
]

# DB API 2.0 type objects
import datetime
import decimal

STRING = type("STRING", (), {})()
BINARY = type("BINARY", (), {})()
NUMBER = type("NUMBER", (), {})()
DATE = datetime.date
TIME = datetime.time
DATETIME = datetime.datetime
DECIMAL = decimal.Decimal

# DB API 2.0 threadsafety
threadsafety = 1  # Threads may share module but not connections
apilevel = "2.0"
paramstyle = "qmark"  # Question mark style parameter markers
