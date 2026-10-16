-- Privileges and existence are checked again after the relation lock is
-- acquired, so changes committed while a caller waits for that lock take
-- effect.  A dblink session plays the waiting caller; this session holds
-- the conflicting lock and commits the change once the caller is queued.
CREATE EXTENSION pg_textsearch;
CREATE EXTENSION dblink;
CREATE ROLE index_info_waiter;
CREATE ROLE index_info_new_owner;

SET client_min_messages = warning;
CREATE TABLE index_info_wait_docs (body text);
INSERT INTO index_info_wait_docs VALUES ('alpha');
CREATE INDEX index_info_wait_idx ON index_info_wait_docs USING bm25(body)
    WITH (text_config = 'english');
CREATE TABLE index_info_wait_owned (body text);
INSERT INTO index_info_wait_owned VALUES ('alpha');
CREATE INDEX index_info_wait_owned_idx ON index_info_wait_owned
    USING bm25(body) WITH (text_config = 'english');
CREATE TABLE index_info_wait_dropped (body text);
INSERT INTO index_info_wait_dropped VALUES ('alpha');
CREATE INDEX index_info_wait_dropped_idx ON index_info_wait_dropped
    USING bm25(body) WITH (text_config = 'english');
RESET client_min_messages;
ALTER TABLE index_info_wait_owned OWNER TO index_info_waiter;
GRANT MAINTAIN ON index_info_wait_docs, index_info_wait_dropped
    TO index_info_waiter;

SELECT dblink_connect('waiter', format(
    'host=%s port=%s dbname=%s',
    current_setting('unix_socket_directories'),
    current_setting('port'), current_database()));
CREATE TEMP TABLE index_info_waiter_pid AS
SELECT pid FROM dblink('waiter', 'SELECT pg_backend_pid()') AS t(pid int);
SELECT dblink_exec('waiter', 'SET ROLE index_info_waiter');

-- Wait until the waiter has passed its first check and is queued for the
-- relation lock.  Reaching the queue proves the first check passed, so a
-- later error comes from the check made after the lock is acquired.
CREATE FUNCTION index_info_wait_for_lock(rel oid) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
    deadline timestamptz := clock_timestamp() + interval '30 seconds';
BEGIN
    LOOP
        IF EXISTS (
            SELECT 1 FROM pg_locks
            JOIN index_info_waiter_pid USING (pid)
            WHERE relation = rel AND NOT granted
        ) THEN
            RETURN;
        END IF;
        IF dblink_is_busy('waiter') = 0 THEN
            RAISE EXCEPTION 'waiter finished without waiting for the lock';
        END IF;
        IF clock_timestamp() >= deadline THEN
            RAISE EXCEPTION 'timed out waiting for the waiter to queue';
        END IF;
        PERFORM pg_sleep(0.01);
    END LOOP;
END
$$;

-- MAINTAIN revoked while waiting.
BEGIN;
REVOKE MAINTAIN ON index_info_wait_docs FROM index_info_waiter;
SET LOCAL client_min_messages = warning;
REINDEX INDEX index_info_wait_idx;
SELECT dblink_send_query('waiter',
    $$SELECT * FROM bm25_index_info('index_info_wait_idx')$$);
SELECT index_info_wait_for_lock('index_info_wait_idx'::regclass);
COMMIT;
SELECT * FROM dblink_get_result('waiter', false)
    AS t(v bigint, c bigint, a boolean, l boolean);
SELECT * FROM dblink_get_result('waiter', false)
    AS t(v bigint, c bigint, a boolean, l boolean);

-- Ownership changed while waiting, for an owner-only caller.
BEGIN;
ALTER TABLE index_info_wait_owned OWNER TO index_info_new_owner;
SET LOCAL client_min_messages = warning;
REINDEX INDEX index_info_wait_owned_idx;
SELECT dblink_send_query('waiter',
    $$SELECT bm25_compact('index_info_wait_owned_idx')$$);
SELECT index_info_wait_for_lock('index_info_wait_owned_idx'::regclass);
COMMIT;
SELECT * FROM dblink_get_result('waiter', false) AS t(r text);
SELECT * FROM dblink_get_result('waiter', false) AS t(r text);

-- Dropped while waiting: a clean "does not exist", not an internal error.
-- The message contains the OID, so compare it rather than print it.
SELECT 'index_info_wait_dropped_idx'::regclass::oid AS dropped_oid \gset
BEGIN;
DROP INDEX index_info_wait_dropped_idx;
SELECT dblink_send_query('waiter',
    format('SELECT * FROM bm25_index_info(%s::oid::regclass)', :dropped_oid));
SELECT index_info_wait_for_lock(:dropped_oid);
COMMIT;
SET client_min_messages = warning;
SELECT * FROM dblink_get_result('waiter', false)
    AS t(v bigint, c bigint, a boolean, l boolean);
RESET client_min_messages;
SELECT rtrim(dblink_error_message('waiter'), E'\n') =
       format('ERROR:  relation with OID %s does not exist', :dropped_oid)
    AS dropped_error;
SELECT * FROM dblink_get_result('waiter', false)
    AS t(v bigint, c bigint, a boolean, l boolean);

SELECT dblink_disconnect('waiter');
DROP FUNCTION index_info_wait_for_lock(oid);
DROP TABLE index_info_wait_docs, index_info_wait_owned,
    index_info_wait_dropped;
DROP ROLE index_info_waiter;
DROP ROLE index_info_new_owner;
DROP EXTENSION dblink;
DROP EXTENSION pg_textsearch;
