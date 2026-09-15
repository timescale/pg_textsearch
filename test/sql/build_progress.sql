-- Test BM25 build-progress lifecycle.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Progress notices should be emitted only when an index is actually created.
CREATE TABLE progress_noop_docs (content text);
CREATE INDEX progress_noop_idx ON progress_noop_docs USING bm25(content)
    WITH (text_config = 'english');
CREATE INDEX IF NOT EXISTS progress_noop_idx
    ON progress_noop_docs USING bm25(content)
    WITH (text_config = 'english');

CREATE TABLE progress_actual_docs (content text);
CREATE TABLE progress_collision_docs (id integer);
CREATE INDEX progress_actual_idx ON progress_collision_docs(id);

CREATE FUNCTION progress_drop_collision()
RETURNS event_trigger AS $$
BEGIN
    IF current_setting('progress_test.drop_collision', true) = 'on' THEN
        PERFORM set_config('progress_test.drop_collision', 'off', true);
        EXECUTE 'DROP INDEX progress_actual_idx';
    END IF;
END
$$ LANGUAGE plpgsql;

CREATE EVENT TRIGGER progress_drop_collision_trigger
ON ddl_command_start
WHEN TAG IN ('CREATE INDEX')
EXECUTE FUNCTION progress_drop_collision();

SET progress_test.drop_collision = on;
CREATE INDEX IF NOT EXISTS progress_actual_idx
    ON progress_actual_docs USING bm25(content)
    WITH (text_config = 'english');
RESET progress_test.drop_collision;

SELECT indrelid = 'progress_actual_docs'::regclass AS actual_index_created
FROM pg_index
WHERE indexrelid = 'progress_actual_idx'::regclass;

-- A failed build must not leave progress active for later implicit builds.
CREATE TABLE progress_abort_docs (
    id integer,
    content text
) PARTITION BY RANGE (id);
CREATE TABLE progress_abort_part
    PARTITION OF progress_abort_docs
    FOR VALUES FROM (0) TO (10);

CREATE FUNCTION progress_fail_build_end()
RETURNS event_trigger AS $$
BEGIN
    IF current_setting('progress_test.fail_build_end', true) = 'on' THEN
        PERFORM set_config('progress_test.fail_build_end', 'off', true);
        RAISE EXCEPTION 'forced progress abort';
    END IF;
END
$$ LANGUAGE plpgsql;

CREATE EVENT TRIGGER progress_fail_build_end_trigger
ON ddl_command_end
WHEN TAG IN ('CREATE INDEX')
EXECUTE FUNCTION progress_fail_build_end();

SET progress_test.fail_build_end = on;
\set VERBOSITY terse
CREATE INDEX progress_abort_idx
    ON progress_abort_docs USING bm25(content)
    WITH (text_config = 'english');
\set VERBOSITY default
RESET progress_test.fail_build_end;

REINDEX INDEX progress_actual_idx;

-- Nested index DDL must not replace an outer partitioned build's progress.
CREATE TABLE progress_nested_docs (
    id integer,
    content text
) PARTITION BY RANGE (id);
CREATE TABLE progress_nested_part_a
    PARTITION OF progress_nested_docs
    FOR VALUES FROM (0) TO (10);
CREATE TABLE progress_nested_part_b
    PARTITION OF progress_nested_docs
    FOR VALUES FROM (10) TO (20);
CREATE TABLE progress_nested_side (content text);
CREATE TABLE progress_nested_reindex_side (content text);
CREATE INDEX progress_nested_reindex_idx
    ON progress_nested_reindex_side USING bm25(content)
    WITH (text_config = 'english');

CREATE FUNCTION progress_create_nested_index()
RETURNS event_trigger AS $$
DECLARE
    action text := current_setting('progress_test.nested_action', true);
BEGIN
    IF action = 'create' THEN
        PERFORM set_config('progress_test.nested_action', 'running', true);
        EXECUTE 'CREATE INDEX progress_nested_side_idx '
                'ON progress_nested_side USING bm25(content) '
                'WITH (text_config = ''english'')';
    ELSIF action = 'reindex' THEN
        PERFORM set_config('progress_test.nested_action', 'running', true);
        EXECUTE 'REINDEX INDEX progress_nested_reindex_idx';
    END IF;
END
$$ LANGUAGE plpgsql;

CREATE EVENT TRIGGER progress_create_nested_trigger
ON ddl_command_end
WHEN TAG IN ('CREATE INDEX')
EXECUTE FUNCTION progress_create_nested_index();

SET progress_test.nested_action = 'create';
CREATE INDEX progress_nested_idx
    ON progress_nested_docs USING bm25(content)
    WITH (text_config = 'english');
RESET progress_test.nested_action;

CREATE TABLE progress_reindex_docs (
    id integer,
    content text
) PARTITION BY RANGE (id);
CREATE TABLE progress_reindex_part_a
    PARTITION OF progress_reindex_docs
    FOR VALUES FROM (0) TO (10);
CREATE TABLE progress_reindex_part_b
    PARTITION OF progress_reindex_docs
    FOR VALUES FROM (10) TO (20);

SET progress_test.nested_action = reindex;
CREATE INDEX progress_reindex_outer_idx
    ON progress_reindex_docs USING bm25(content)
    WITH (text_config = 'english');
RESET progress_test.nested_action;

DROP EVENT TRIGGER progress_create_nested_trigger;
DROP FUNCTION progress_create_nested_index();
DROP EVENT TRIGGER progress_fail_build_end_trigger;
DROP FUNCTION progress_fail_build_end();
DROP EVENT TRIGGER progress_drop_collision_trigger;
DROP FUNCTION progress_drop_collision();
DROP TABLE progress_noop_docs;
DROP TABLE progress_actual_docs;
DROP TABLE progress_collision_docs;
DROP TABLE progress_abort_docs;
DROP TABLE progress_nested_docs;
DROP TABLE progress_nested_side;
DROP TABLE progress_nested_reindex_side;
DROP TABLE progress_reindex_docs;
DROP EXTENSION pg_textsearch CASCADE;
