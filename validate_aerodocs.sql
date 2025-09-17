-- Enable score logging for validation
SET tapir.log_scores = true;

-- Run the aerodocs test
\i test/sql/aerodocs.sql
