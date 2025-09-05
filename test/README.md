# Tapir Extension Test Suite

This directory contains comprehensive tests for the Tapir PostgreSQL extension.

## Directory Structure

```
test/
├── sql/               # SQL regression test files
├── expected/          # Expected output files for regression tests
├── results/           # Generated test results (temporary)
├── scripts/           # Test utilities and helper scripts
└── README.md         # This file
```

## Test Types

### 1. SQL Regression Tests (`make installcheck`)

**Location:** `test/sql/*.sql` with corresponding `test/expected/*.out`

- **basic.sql** - Basic extension functionality and type operations
- **index.sql** - Index creation, insertion, and basic operations
- **vector.sql** - tpvector type and BM25 scoring functionality
- **queries.sql** - Complex query patterns and performance tests
- **aerodocs.sql** - Information retrieval validation using aerodynamics documents
- **mixed.sql** - Mixed operations testing (formerly concurrent.sql)
- **recovery.sql** - Index recovery after simulated crashes
- **strings.sql** - String handling and edge case testing
- **memory_limits.sql** - Memory budget enforcement and per-index limits testing

### 2. Shell-based Tests

**Location:** `test/scripts/`

- **recovery.sh** - Actual crash simulation with SIGKILL and recovery verification
- **concurrency.sh** - Multi-session concurrency and string interning safety tests
- **memory_limits.sh** - Memory budget enforcement stress testing with capacity limits

### 3. Concurrency Testing

The Tapir extension includes comprehensive concurrency testing to verify thread-safety of shared memory structures, string interning, and posting list management.

#### Concurrency Tests (`concurrency.sh`)
- Concurrent index creation safety
- Multi-session inserts with string interning consistency 
- Mixed operations (insert/update/search/delete)
- Hash table integrity under concurrent load
- High-load concurrent operations (8+ sessions)
- Concurrent string interning with identical terms

Tests verify:
- ✅ No PostgreSQL crashes or segmentation faults
- ✅ String interning hash table consistency
- ✅ Posting list thread safety
- ✅ Shared memory structure integrity
- ✅ Proper transaction isolation

### 4. Memory Budget Testing

The Tapir extension enforces strict per-index memory budgets to prevent runaway memory usage.

#### Memory Limits Tests (`memory_limits.sh`)
- **String Table Capacity**: Tests hash table entry limits (25% of budget)
- **Posting List Capacity**: Tests posting entry limits (75% of budget)  
- **Mixed Scenarios**: Tests balanced usage of both limits
- **Per-Index Budgets**: Verifies independent budgets for multiple indexes
- **Error Handling**: Validates clear error messages when limits exceeded

Configuration:
- Uses `tapir.shared_memory_size = 1` (1MB per index) for fast testing
- Assumes 5-character average term length
- String table: ~6,900 entry limit, Posting lists: ~32,000 entry limit

Tests verify:
- ✅ Hard capacity limits prevent memory budget violations
- ✅ Clear error messages when limits exceeded
- ✅ Per-index budgets work independently
- ✅ Graceful transaction abort on capacity exceeded

## Running Tests

### All Tests
```bash
make installcheck
```

### Individual Tests
```bash
make installcheck REGRESS=basic
make installcheck REGRESS=mixed
make installcheck REGRESS=memory_limits
```

### Shell Scripts
```bash
# Individual shell script targets
make test-concurrency      # Multi-session concurrency safety
make test-recovery         # Crash recovery testing
make test-memory-limits    # Memory budget enforcement stress testing
make test-shell            # All shell scripts (concurrency + recovery + memory limits)
make test-all              # Complete test suite (SQL + shell scripts)

# Direct execution
cd test/scripts
./concurrency.sh          # Multi-session concurrency safety
./recovery.sh             # Crash recovery testing
./memory_limits.sh        # Memory budget enforcement stress testing
```

## Test Development Guidelines

### Adding New SQL Tests

1. Create `test/sql/mytest.sql`
2. Run the test to generate output: `make installcheck REGRESS=mytest`
3. If output looks correct, copy to expected: `cp test/results/mytest.out test/expected/`
4. Add `mytest` to `REGRESS` variable in `Makefile`

### Test Data Patterns

- Use meaningful test data that reflects real-world usage
- Include edge cases (empty results, single terms, very long documents)
- Test error conditions and boundary cases
- Verify BM25 scores are reasonable (typically 0-10 range)

### Concurrency Testing

**SQL-level Testing:**
- Use transactions to simulate concurrent operations
- Test posting list updates with overlapping terms
- Verify string interning consistency
- Test multiple index operations

**Shell-based Real Concurrency Testing:**
- Use multiple PostgreSQL sessions for true concurrency
- Test high-load scenarios with 8+ concurrent connections
- Verify thread safety of shared memory structures
- Test edge cases like identical string interning from multiple sessions
- Validate crash recovery under concurrent load

**Key Concurrency Concerns:**
- String interning hash table race conditions
- Posting list memory management
- Shared memory corruption under high load
- Transaction isolation with custom index operations

### Performance Testing

- Include stress tests with bulk data (50+ documents)
- Test search performance with different query patterns
- Verify memory usage stays within limits
- Test index rebuild scenarios

## Test Infrastructure

### Custom PostgreSQL Instance

Tests run on a temporary PostgreSQL instance with:
- Port: 55433 (to avoid conflicts)
- Shared preload: `tapir` extension
- Enhanced logging for debugging
- Controlled shared memory settings

### Memory Leak Detection

Tests monitor for:
- Hash table scan leaks (`hash_seq_search` without `hash_seq_term`)
- Posting list memory growth
- String interning table consistency

### Error Patterns to Watch

1. **Resource Leaks:** "leaked hash_seq_search scan"
2. **Crashes:** "PANIC: ERRORDATA_STACK_SIZE exceeded"
3. **Recovery Failures:** Database won't accept connections after crash
4. **Score Inconsistencies:** BM25 scores outside expected ranges

## Debugging Failed Tests

### Check Regression Diffs
```bash
cat test/regression.diffs
```

### Run Individual Test with Verbose Output
```bash
make installcheck REGRESS=problematic_test
```

### Check PostgreSQL Logs
```bash
tail -f tmp_check_shared/data/logfile
```

### Memory and Resource Debugging
Look for WARNING messages about resource leaks in test output.

## Integration with CI/CD

Tests are designed to:
- Run deterministically in any environment  
- Clean up all resources automatically
- Provide clear pass/fail indicators
- Generate detailed failure diagnostics

### Recommended CI/CD Test Flow
```bash
make test-all              # Complete test suite (recommended for CI)
# OR separate stages:
make installcheck          # SQL regression tests (fast)
make test-concurrent-basic # Basic concurrency tests (medium)  
make test-recovery        # Crash recovery tests (slower)
```

### Test Performance
- **SQL regression tests**: ~3-5 seconds (8 tests)
- **Basic concurrency tests**: ~30 seconds (real multi-session)
- **Stress tests**: ~2-3 minutes (high load validation)
- **Recovery tests**: ~30 seconds (crash simulation)

The test suite validates both functional correctness and system reliability under various operational conditions.