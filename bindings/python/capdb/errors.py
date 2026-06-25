"""DB API 2.0 exception classes."""


class Error(Exception):
    """Base exception for all CapDB errors."""

    pass


class InterfaceError(Error):
    """Exception raised for errors related to the database interface."""

    pass


class DatabaseError(Error):
    """Exception raised for errors related to the database."""

    pass


class DataError(DatabaseError):
    """Exception raised for errors related to processed data."""

    pass


class OperationalError(DatabaseError):
    """Exception raised for errors related to database operation."""

    pass


class IntegrityError(DatabaseError):
    """Exception raised when integrity constraint is violated."""

    pass


class InternalError(DatabaseError):
    """Exception raised for internal database errors."""

    pass


class ProgrammingError(DatabaseError):
    """Exception raised for programming errors."""

    pass


class NotSupportedError(DatabaseError):
    """Exception raised when a method is not supported."""

    pass
