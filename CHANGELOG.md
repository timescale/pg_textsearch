# Changelog

All notable changes to pg_textsearch will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [0.0.4] - 2024-11-19

### Added
- PostgreSQL 18 support
- BM25 score validation system for improved accuracy

### Fixed
- NULL pointer dereference in UPDATE operations (#33)
- IDF calculation now correctly handles term frequency across corpus (#30)
- Release workflow now produces correct binaries for each PostgreSQL version (#32)
- Package release workflow correctly uses version-specific pg_config (#31)

### Changed
- Release artifacts now include PostgreSQL 18 binaries
- Improved release workflow reliability

## [0.0.3] - 2024-11-06

### Added
- Shared memory accounting and limit enforcement
- Crash recovery via document ID pages
- Comprehensive concurrency and recovery tests

### Changed
- Complete string-based architecture implementation
- Repository renamed from tapir to pg_textsearch

## [0.0.2] - 2024-10-28

### Fixed
- Release workflow artifacts and permissions

### Security
- Prevented BM25 index creation on tables with inheritance

## [0.0.1] - 2024-10-21

### Added
- Initial release with memtable-only BM25 implementation
- Full-text search with BM25 ranking
- PostgreSQL 17 support
- Basic index operations and scoring
