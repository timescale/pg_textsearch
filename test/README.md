# Tapir Extension Test Suite

This directory contains tests for the Tapir PostgreSQL extension.

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
- **limits.sql** - Memory budget enforcement and per-index limits testing

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

### 4. Stress Testing

Long-running stress tests for extended operation verification and resource leak detection.
All tests are time-based to ensure sustained load over the configured duration.

#### Stress Tests (`stress.sh`)
- **Test 1: Extended Insert/Query** (40% of duration) - Continuous inserts (~30K docs/min) with concurrent queries
- **Test 2: Concurrent Stress** (25% of duration) - Multiple readers querying while writer inserts continuously
- **Test 3: Multiple Indexes** (15% of duration) - 3 indexes with interleaved insert/query operations
- **Test 4: Segment Stress** (10% of duration) - Forces frequent spills to create many segments
- **Test 5: Resource Cleanup** (10% of duration) - Repeated index creation/deletion cycles

Configuration (environment variables):
- `STRESS_DURATION_MINUTES` - Test duration (default: 5 local, 30 nightly)
- `SPILL_THRESHOLD` - Posting threshold for spills (default: 1000 local, 500 nightly)
- `DOCS_PER_BATCH` - Documents per insert batch (default: 100)
- `CONCURRENT_READERS` - Number of concurrent reader processes (default: 3)

Expected load for 30-minute nightly run:
- ~1.2 million documents inserted
- ~1,500 segments created
- ~40,000 queries executed
- 4+ concurrent database connections

Tests verify:
- ✅ Continued correctness over extended operation
- ✅ No memory leaks under sustained load
- ✅ Proper segment creation and querying
- ✅ Resource cleanup on index drop
- ✅ Segment count tracking and validation

### 5. Memory Budget Testing

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
make installcheck REGRESS=limits
```

### Shell Scripts
```bash
# Individual shell script targets
make test-concurrency      # Multi-session concurrency safety
make test-recovery         # Crash recovery testing
make test-stress           # Long-running stress tests
make test-shell            # All shell scripts (concurrency + recovery + memory limits)
make test-all              # Complete test suite (SQL + shell scripts)

# Direct execution
cd test/scripts
./concurrency.sh          # Multi-session concurrency safety
./recovery.sh             # Crash recovery testing
./stress.sh               # Long-running stress tests (configurable duration)

# Stress test with custom duration (10 minutes)
STRESS_DURATION_MINUTES=10 ./stress.sh
```

## Test Development Guidelines

### Adding New SQL Tests

1. Create `test/sql/mytest.sql`
2. Run the test to generate output: `make installcheck REGRESS=mytest`
3. If output looks correct, copy to expected: `cp test/results/mytest.out test/expected/`
4. Add `mytest` to `REGRESS` variable in `Makefile`

### Test Data Patterns

- Aim for test data that reflects real-world usage
- Include edge cases (empty results, single terms, very long documents)
- Test error conditions and boundary cases
- Check BM25 scores against reference Python implementation

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
make test-concurrency      # Concurrency tests (medium)
make test-recovery         # Crash recovery tests (slower)
make test-stress           # Long-running stress tests (nightly)
```

### Nightly Stress Tests

The `nightly-stress.yml` workflow runs extended stress tests with:
- AddressSanitizer with leak detection enabled
- Low spill thresholds to create many segments
- Extended duration tests (10+ minutes)
- Automatic issue creation on failure

These tests are designed to catch:
- Memory leaks that only manifest under sustained load
- Resource leaks from repeated index operations
- Correctness issues that appear over time
- Segment handling edge cases
