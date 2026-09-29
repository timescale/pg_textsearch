\pset format unaligned
SET client_min_messages = warning;
CREATE EXTENSION pg_textsearch;
CREATE EXTENSION injection_points;
CREATE EXTENSION dblink;

CREATE TABLE scoring_readers (id int, body text);
CREATE INDEX scoring_readers_idx ON scoring_readers USING bm25(body)
    WITH (text_config = 'simple');
INSERT INTO scoring_readers VALUES (1, 'alpha'), (2, 'needle');
SET enable_seqscan = off;
SELECT id FROM scoring_readers
ORDER BY body <@> to_bm25query('needle', 'scoring_readers_idx') LIMIT 1;

SELECT dblink_connect('reader', format(
    'host=%s port=%s dbname=%s application_name=scoring_reader',
    current_setting('unix_socket_directories'),
    current_setting('port'), current_database()));
SELECT dblink_exec('reader', 'SET enable_seqscan = off');
SELECT * FROM dblink('reader',
    'SELECT injection_points_set_local()') AS t(result text);
SELECT * FROM dblink('reader',
    $$SELECT injection_points_attach('pg-textsearch-scoring-source', 'wait')$$
) AS t(result text);
SELECT dblink_send_query('reader', $q$
    SELECT id FROM scoring_readers
    ORDER BY body <@> to_bm25query('alpha', 'scoring_readers_idx') LIMIT 1
$q$);
DO $$
BEGIN
    FOR attempt IN 1..500 LOOP
        PERFORM pg_stat_clear_snapshot();
        IF EXISTS (
            SELECT 1 FROM pg_stat_activity
            WHERE application_name = 'scoring_reader'
              AND wait_event = 'pg-textsearch-scoring-source'
        ) THEN
            RETURN;
        END IF;
        PERFORM pg_sleep(0.01);
    END LOOP;
    RAISE EXCEPTION 'reader did not reach scoring pause';
END
$$;

-- A reader of the unchanged cache must finish while scoring is paused.
SELECT dblink_connect('fast', format(
    'host=%s port=%s dbname=%s',
    current_setting('unix_socket_directories'),
    current_setting('port'), current_database()));
SELECT dblink_exec('fast', 'SET enable_seqscan = off');
SELECT dblink_send_query('fast', $q$
    SELECT id FROM scoring_readers
    ORDER BY body <@> to_bm25query('needle', 'scoring_readers_idx') LIMIT 1
$q$);
DO $$
BEGIN
    FOR attempt IN 1..500 LOOP
        EXIT WHEN dblink_is_busy('fast') = 0;
        PERFORM pg_sleep(0.01);
    END LOOP;
END
$$;
SELECT dblink_is_busy('fast') AS blocked;
SELECT injection_points_wakeup('pg-textsearch-scoring-source');
SELECT * FROM dblink_get_result('fast') AS t(id int);
SELECT * FROM dblink_get_result('fast') AS t(id int);
SELECT dblink_disconnect('fast');
SELECT * FROM dblink_get_result('reader') AS t(id int);
SELECT * FROM dblink_get_result('reader') AS t(id int);
SELECT dblink_disconnect('reader');

DROP TABLE scoring_readers;
DROP EXTENSION dblink;
DROP EXTENSION injection_points;
DROP EXTENSION pg_textsearch;
