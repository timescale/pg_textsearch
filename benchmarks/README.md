# Tapir Benchmarks

This directory contains benchmark tests for the Tapir PostgreSQL extension.

## Available Benchmarks

### Memory Stress Test (`memory_stress.sql`)

Tests the extension's behavior under memory pressure by creating a large dataset and attempting to index it.

**Purpose**: 
- Identify memory limits and exhaustion behavior
- Test performance with large document collections
- Validate error handling under resource constraints

**Features**:
- Generates 100K documents with varied, realistic content
- Creates complex documents with different vocabulary patterns
- Tests index creation and query performance under load
- Demonstrates memory allocation patterns

**Usage**:
```bash
# Run with default settings (64MB shared memory)
./run_memory_stress.sh

# Or run directly with psql
psql -f sql/memory_stress.sql
```

**To test with minimal memory**:
1. Add to `postgresql.conf`: `tapir.shared_memory_size = 1`
2. Restart PostgreSQL
3. Run the benchmark

**Expected Behavior**:
- With sufficient memory: Index creation succeeds, queries complete
- With insufficient memory: Index creation fails with memory errors
- Shows actual memory usage patterns and limits

### Cranfield Collection Benchmark (`cranfield/`)

Standard information retrieval test collection with 1400 aerodynamics abstracts and 225 queries.

**Purpose**: 
- Validate BM25 implementation quality against standard IR benchmark
- Performance testing with realistic document collection
- Search quality evaluation using expected relevance rankings

**Usage**:
```bash
# Run complete benchmark (data loading + queries)
./run_cranfield.sh

# Or run phases individually
cd sql/cranfield/
psql -f 01-load.sql    # Load 1400 documents + create index  
psql -f 02-queries.sql  # Run 225 queries with timing
```

**Expected Performance**:
- Data loading: 10-60 seconds (includes index creation)
- Query execution: 30-120 seconds (225 queries)
- Memory usage: Scales with tapir.shared_memory_size setting

## Running Benchmarks

**Memory Stress Test**:
```bash
./run_memory_stress.sh
# Or directly: psql -f sql/memory_stress.sql
```

**Cranfield Benchmark**:
```bash
./run_cranfield.sh
# Or directly: cd sql/cranfield/ && psql -f 01-load.sql && psql -f 02-queries.sql
```

## Interpreting Results

### Memory Stress Test

**Success indicators**:
- Index creation completes
- All queries return results
- No memory allocation failures

**Stress indicators**:
- Slow index creation (>30 seconds for 100K docs)
- Query timeouts or errors
- Memory allocation warnings in PostgreSQL logs

**Failure indicators**:
- "out of memory" errors during index creation
- Index creation aborts
- Connection drops due to memory exhaustion

### Performance Expectations

With 64MB shared memory (default):
- 100K documents should index successfully
- Index creation: ~10-60 seconds
- Simple queries: <1 second
- Complex queries: 1-5 seconds

With 1MB shared memory:
- Should fail or show severe memory pressure
- Demonstrates extension limits

## Customization

Modify document count in `memory_stress.sql`:
```sql
FROM generate_series(1, 200000) AS i;  -- 200K for more stress
```

Adjust content complexity by modifying the CASE statements that generate document content.

## Notes

- `tapir.shared_memory_size` is a PGC_POSTMASTER parameter requiring PostgreSQL restart
- Large datasets require sufficient disk space for the test table
- Monitor system memory usage during benchmarks
- Clean up is automatic but can be done manually: `DROP TABLE stress_docs;`