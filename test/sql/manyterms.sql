-- Test hash table automatic resizing with many terms
-- This test creates enough unique terms to trigger hash table resize
-- and verifies the system continues to work correctly

CREATE EXTENSION tapir;

-- Create test table
CREATE TABLE manyterms_test (id serial, content text);

-- Create index to initialize hash table with small initial size
CREATE INDEX manyterms_idx ON manyterms_test USING tapir(content) WITH (text_config='english');

-- Add enough unique terms to exceed initial hash table capacity
-- With 64 buckets at 0.75 load factor, >48 unique terms should trigger automatic resize
INSERT INTO manyterms_test (content) VALUES
('alpha'), ('beta'), ('gamma'), ('delta'), ('epsilon'), ('zeta'), ('eta'), ('theta'),
('iota'), ('kappa'), ('lambda'), ('mu'), ('nu'), ('xi'), ('omicron'), ('pi'),
('rho'), ('sigma'), ('tau'), ('upsilon'), ('phi'), ('chi'), ('psi'), ('omega'),
('one'), ('two'), ('three'), ('four'), ('five'), ('six'), ('seven'), ('eight'),
('nine'), ('ten'), ('eleven'), ('twelve'), ('thirteen'), ('fourteen'), ('fifteen'), ('sixteen'),
('seventeen'), ('eighteen'), ('nineteen'), ('twenty'), ('twentyone'), ('twentytwo'), ('twentythree'), ('twentyfour'),
('twentyfive'), ('twentysix'), ('twentyseven'), ('twentyeight'), ('twentynine'), ('thirty'), ('thirtyone'), ('thirtytwo'),
('thirtythree'), ('thirtyfour'), ('thirtyfive'), ('thirtysix'), ('thirtyseven'), ('thirtyeight'), ('thirtynine'), ('forty'),
('fortyone'), ('fortytwo'), ('fortythree'), ('fortyfour'), ('fortyfive'), ('fortysix'), ('fortyseven'), ('fortyeight'),
('fortynine'), ('fifty'), ('fiftyone'), ('fiftytwo'), ('fiftythree'), ('fiftyfour'), ('fiftyfive'), ('fiftysix');

-- Verify we can count all documents successfully (no errors from resize)
SELECT COUNT(*) as total_docs FROM manyterms_test;

-- Test searching for specific terms works correctly after resize
SELECT id, content FROM manyterms_test WHERE content = 'alpha' ORDER BY id LIMIT 1;
SELECT id, content FROM manyterms_test WHERE content = 'omega' ORDER BY id LIMIT 1;

-- Test BM25 scoring still works after hash table resize
SELECT id, content, content <@> to_tpquery('fifty', 'manyterms_idx') as score
FROM manyterms_test
WHERE content = 'fifty'
ORDER BY score
LIMIT 1;

-- Clean up
DROP INDEX manyterms_idx;
DROP TABLE manyterms_test;
