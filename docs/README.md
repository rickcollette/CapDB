# CapDB Documentation

Welcome to the CapDB documentation. This directory contains comprehensive guides for using and developing with CapDB.

## Quick Links

- **[Getting Started](GETTING_STARTED.md)** - Installation and first steps
- **[Language Drivers](../LANGUAGE_DRIVERS.md)** - Go, Rust, Python driver overview
- **[Drivers Guide](DRIVERS_GUIDE.md)** - Comprehensive driver usage guide with examples
- **[Building CapDB](../BUILD.md)** - Build instructions and options
- **[Architecture](ARCHITECTURE.md)** - System design and component overview
- **[Bindings & Build Tools](../BINDINGS_BUILD.md)** - Build helpers and pkg-config

## Documentation by Role

### For End Users

Start here if you want to use CapDB in your application:

1. Read [Getting Started](GETTING_STARTED.md)
2. Choose your language driver and follow [Drivers Guide](DRIVERS_GUIDE.md)
3. Refer to [Language Drivers](../LANGUAGE_DRIVERS.md) for API reference

### For Developers

Start here if you want to contribute to CapDB:

1. Read [Architecture](ARCHITECTURE.md)
2. Follow [Build Instructions](../BUILD.md)
3. Check [Development Guide](DEVELOPMENT.md)
4. See [Contributing Guidelines](../CONTRIBUTING.md)

### For DevOps/Operators

Start here if you're deploying CapDB:

1. Follow [Deployment Guide](DEPLOYMENT.md)
2. Review [Configuration](CONFIGURATION.md)
3. Check [Monitoring & Troubleshooting](MONITORING.md)

## Table of Contents

### User Guides
- [Getting Started](GETTING_STARTED.md) - Installation and quick start
- [Drivers Guide](DRIVERS_GUIDE.md) - Language-specific usage patterns
- [Configuration](CONFIGURATION.md) - CapDB server and client configuration

### Developer Documentation
- [Architecture](ARCHITECTURE.md) - System design overview
- [Development Guide](DEVELOPMENT.md) - Setting up dev environment
- [Building](../BUILD.md) - Build system and CMake configuration
- [Contributing](../CONTRIBUTING.md) - Contributing guidelines

### Operations Documentation
- [Deployment Guide](DEPLOYMENT.md) - Production deployment
- [Monitoring](MONITORING.md) - Performance monitoring and debugging
- [Troubleshooting](TROUBLESHOOTING.md) - Common issues and solutions
- [Security](../SECURITY.md) - Security considerations

### Reference Documentation
- [Language Drivers Overview](../LANGUAGE_DRIVERS.md) - Multi-language driver support
- [Build Tools & Pkg-Config](../BINDINGS_BUILD.md) - Integration with build systems
- [DSN Format](DSN_FORMAT.md) - Connection string reference
- [SQL Support](SQL_SUPPORT.md) - Supported SQL features
- [API Reference](API_REFERENCE.md) - C API documentation

## Key Features

- **Network Protocol** - TLS-secured remote access with connection pooling
- **Embedded Mode** - SQLite-compatible embedded database
- **Replication** - Primary/replica setup for high availability
- **Connection Pooling** - Server-side pool management
- **Multi-Language Support** - Go, Rust, Python drivers included

## Getting Help

- **Documentation**: See relevant guides above
- **Issues**: [GitHub Issues](https://github.com/rickcollette/CapDB/issues)
- **Discussions**: [GitHub Discussions](https://github.com/rickcollette/CapDB/discussions)
- **Examples**: See `bindings/*/example/` directories

## Release Notes

See [CHANGELOG.md](../CHANGELOG.md) for version history and breaking changes.

## License

CapDB is licensed under the MIT License. See [LICENSE](../LICENSE) for details.
