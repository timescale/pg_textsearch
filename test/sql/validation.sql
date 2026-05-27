-- BM25 Score Validation Functions
--
-- Formula: BM25(q,d) = Σ IDF(qi) * (tf(qi,d) * (k1 + 1)) / (tf(qi,d) + k1 * (1 - b + b * |d| / avgdl))
-- Where:
--   IDF(qi) = ln(1 + (N - df(qi) + 0.5) / (df(qi) + 0.5))
--   tf(qi,d) = term frequency of qi in document d
--   |d| = document length (quantized via fieldnorm), avgdl = average document length
--   k1 = term frequency saturation, b = length normalization
--
-- Note: Document lengths are quantized using Lucene's SmallFloat encoding scheme
-- (fieldnorm) which maps lengths to 256 buckets. This matches Tapir's internal
-- representation for consistent scoring between index scans and operator evaluation.
--
-- Usage Example:
--   SELECT validate_bm25_scoring('mytable', 'content_column', 'my_bm25_idx', 'search query', 'english', 1.2, 0.75)

-- Fieldnorm decode table (Lucene SmallFloat.byte4ToInt)
-- Values 0-39 are exact, larger values use exponential encoding
CREATE OR REPLACE FUNCTION fieldnorm_quantize(doc_length integer)
RETURNS integer AS $$
DECLARE
    decode_table integer[] := ARRAY[
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        32, 33, 34, 35, 36, 37, 38, 39,
        40, 42, 44, 46, 48, 50, 52, 54,
        56, 60, 64, 68, 72, 76, 80, 84,
        88, 96, 104, 112, 120, 128, 136, 144,
        152, 168, 184, 200, 216, 232, 248, 264,
        280, 312, 344, 376, 408, 440, 472, 504,
        536, 600, 664, 728, 792, 856, 920, 984,
        1048, 1176, 1304, 1432, 1560, 1688, 1816, 1944,
        2072, 2328, 2584, 2840, 3096, 3352, 3608, 3864,
        4120, 4632, 5144, 5656, 6168, 6680, 7192, 7704,
        8216, 9240, 10264, 11288, 12312, 13336, 14360, 15384,
        16408, 18456, 20504, 22552, 24600, 26648, 28696, 30744,
        32792, 36888, 40984, 45080, 49176, 53272, 57368, 61464,
        65560, 73752, 81944, 90136, 98328, 106520, 114712, 122904,
        131096, 147480, 163864, 180248, 196632, 213016, 229400, 245784,
        262168, 294936, 327704, 360472, 393240, 426008, 458776, 491544,
        524312, 589848, 655384, 720920, 786456, 851992, 917528, 983064,
        1048600, 1179672, 1310744, 1441816, 1572888, 1703960, 1835032, 1966104,
        2097176, 2359320, 2621464, 2883608, 3145752, 3407896, 3670040, 3932184,
        4194328, 4718616, 5242904, 5767192, 6291480, 6815768, 7340056, 7864344,
        8388632, 9437208, 10485784, 11534360, 12582936, 13631512, 14680088, 15728664,
        16777240, 18874392, 20971544, 23068696, 25165848, 27263000, 29360152, 31457304,
        33554456, 37748760, 41943064, 46137368, 50331672, 54525976, 58720280, 62914584,
        67108888, 75497496, 83886104, 92274712, 100663320, 109051928, 117440536, 125829144,
        134217752, 150994968, 167772184, 184549400, 201326616, 218103832, 234881048, 251658264,
        268435480, 301989912, 335544344, 369098776, 402653208, 436207640, 469762072, 503316504,
        536870936, 603979800, 671088664, 738197528, 805306392, 872415256, 939524120, 1006632984,
        1073741848, 1207959576, 1342177304, 1476395032, 1610612760, 1744830488, 1879048216, 2013265944
    ];
    lo integer := 1;  -- PostgreSQL arrays are 1-indexed
    hi integer := 256;
    mid integer;
BEGIN
    -- Binary search to find largest index where decode_table[i] <= doc_length
    WHILE lo < hi LOOP
        mid := (lo + hi + 1) / 2;
        IF decode_table[mid] <= doc_length THEN
            lo := mid;
        ELSE
            hi := mid - 1;
        END IF;
    END LOOP;
    -- Return the quantized (decoded) value
    RETURN decode_table[lo];
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION validate_bm25_scoring(
    p_table_name text,
    p_text_column text,
    p_index_name text,
    p_query text,
    p_text_config text,
    p_k1 float,
    p_b float
) RETURNS boolean AS $$
DECLARE
    v_total_docs integer;
    v_avg_doc_length float8;
    v_all_match boolean;
    v_idx_match_10 boolean;
    v_idx_match_100 boolean;
    v_sql text;
    v_idx_sql text;
    v_saved_log_scores text;
BEGIN
    -- Suppress per-row BM25 NOTICEs from this helper's internal index
    -- queries so output stays stable regardless of caller's log_scores
    -- setting, and so the cold+warm doubled execution does not duplicate
    -- NOTICE lines.  log_scores is registered PGC_SUSET; non-superuser
    -- callers cannot change it, so skip the set_config when it's already
    -- off and tolerate insufficient_privilege errors otherwise.
    v_saved_log_scores := current_setting('pg_textsearch.log_scores', true);
    IF v_saved_log_scores IS DISTINCT FROM 'off' THEN
        BEGIN
            PERFORM set_config('pg_textsearch.log_scores', 'off', true);
        EXCEPTION WHEN insufficient_privilege THEN
            -- Caller lacks privilege to mutate the GUC; leave it as-is.
            -- Restore step will also be skipped (v_saved_log_scores -> NULL).
            v_saved_log_scores := NULL;
        END;
    END IF;

    -- Step 1: Tokenize documents with proper term frequency counting
    EXECUTE format($sql$
        CREATE TEMP TABLE temp_docs ON COMMIT DROP AS
        WITH numbered AS (
            SELECT
                ROW_NUMBER() OVER () as doc_id,
                %I as content
            FROM %I
            WHERE %I IS NOT NULL
        ),
        doc_terms AS (
            SELECT
                doc_id,
                content,
                lexeme,
                array_length(string_to_array(positions::text, ','), 1) as term_count
            FROM numbered,
                 LATERAL unnest(to_tsvector(%L, content)) as t(lexeme, positions, weights)
        )
        SELECT
            doc_id,
            content,
            array_agg(lexeme ORDER BY lexeme) as unique_tokens,
            fieldnorm_quantize(SUM(term_count)::integer) as doc_length
        FROM doc_terms
        GROUP BY doc_id, content
    $sql$, p_text_column, p_table_name, p_text_column, p_text_config);

    -- Step 2: Calculate corpus statistics
    SELECT
        COUNT(*),
        AVG(td.doc_length)::float8
    INTO v_total_docs, v_avg_doc_length
    FROM temp_docs td;

    -- Step 3: Build term frequency table with actual counts from positions
    EXECUTE format($sql$
        CREATE TEMP TABLE temp_tf ON COMMIT DROP AS
        WITH numbered AS (
            SELECT ROW_NUMBER() OVER () as doc_id, %I as content
            FROM %I WHERE %I IS NOT NULL
        )
        SELECT
            n.doc_id,
            t.lexeme as term,
            array_length(string_to_array(t.positions::text, ','), 1) as term_freq,
            (SELECT doc_length FROM temp_docs td WHERE td.doc_id = n.doc_id) as doc_length
        FROM numbered n,
             LATERAL unnest(to_tsvector(%L, n.content)) as t(lexeme, positions, weights)
    $sql$, p_text_column, p_table_name, p_text_column, p_text_config);

    -- Step 4: Build document frequency table
    CREATE TEMP TABLE temp_df ON COMMIT DROP AS
    SELECT
        term,
        COUNT(DISTINCT temp_tf.doc_id) as doc_freq
    FROM temp_tf
    GROUP BY term;

    -- Step 5: Tokenize query
    EXECUTE format($sql$
        CREATE TEMP TABLE temp_query ON COMMIT DROP AS
        SELECT DISTINCT lexeme as term
        FROM unnest(to_tsvector(%L, %L))
    $sql$, p_text_config, p_query);

    -- Step 6: Get Tapir scores
    v_sql := format($sql$
        CREATE TEMP TABLE temp_tapir_scores ON COMMIT DROP AS
        SELECT
            ROW_NUMBER() OVER () as doc_id,
            (%I <@> to_bm25query(%L, %L))::float8 as score
        FROM %I
        WHERE %I IS NOT NULL
        ORDER BY ROW_NUMBER() OVER ()
    $sql$, p_text_column, p_query, p_index_name, p_table_name, p_text_column);
    EXECUTE v_sql;

    -- Step 7: Materialize SQL ground-truth scores and compare to standalone
    CREATE TEMP TABLE temp_sql_scores ON COMMIT DROP AS
    WITH sql_scores AS (
        SELECT
            d.doc_id,
            d.content,
            COALESCE(SUM(
                -- IDF: ln(1 + (N - df + 0.5) / (df + 0.5))
                ln(1 + (v_total_docs - df.doc_freq + 0.5)::float8 / (df.doc_freq + 0.5)) *
                -- TF component: (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * |D|/avgdl))
                (tf.term_freq * (p_k1 + 1)) /
                (tf.term_freq + p_k1 * (1 - p_b + p_b * tf.doc_length / v_avg_doc_length))
            ), 0) as bm25_score
        FROM temp_docs d
        CROSS JOIN temp_query q
        LEFT JOIN temp_tf tf ON tf.doc_id = d.doc_id AND tf.term = q.term
        LEFT JOIN temp_df df ON df.term = q.term
        GROUP BY d.doc_id, d.content
    )
    SELECT doc_id, content, bm25_score FROM sql_scores;

    SELECT bool_and(
        -- Compare to 4 decimal places
        ROUND(s.bm25_score::numeric, 4) = ROUND((-t.score)::numeric, 4)
    ) INTO v_all_match
    FROM temp_sql_scores s
    JOIN temp_tapir_scores t ON s.doc_id = t.doc_id;

    -- Step 8: Exercise the index-scan path (top-k ORDER BY + LIMIT) twice
    -- for each of two cap sizes. With memtable_cache_enabled=on, the first
    -- run exercises the cold/lazy-build path and the second the warm cache;
    -- both runs must match the SQL ground-truth scores from Step 7.  With
    -- the cache disabled the two runs are trivially identical so existing
    -- behavior is unchanged.
    v_idx_sql := format($sql$
        SELECT
            %I as content,
            (%I <@> to_bm25query(%L, %L))::float8 as score
        FROM %I
        WHERE %I IS NOT NULL
        ORDER BY %I <@> to_bm25query(%L, %L)
        LIMIT %s
    $sql$, p_text_column, p_text_column, p_query, p_index_name,
          p_table_name, p_text_column,
          p_text_column, p_query, p_index_name, 10);

    EXECUTE format(
        'CREATE TEMP TABLE temp_idx_cold_10 ON COMMIT DROP AS %s', v_idx_sql);
    EXECUTE format(
        'CREATE TEMP TABLE temp_idx_warm_10 ON COMMIT DROP AS %s', v_idx_sql);

    SELECT bool_and(
        -- Cold and warm must produce the same ranked content+score; both
        -- must match the SQL ground-truth score for that content.  Use
        -- IS NOT DISTINCT FROM so one-sided rows (NULL from FULL OUTER
        -- JOIN) are detected instead of being silently masked by NULL
        -- propagation through `=`.
        c.content IS NOT DISTINCT FROM w.content
        AND ROUND(c.score::numeric, 4)
            IS NOT DISTINCT FROM ROUND(w.score::numeric, 4)
        AND EXISTS (
            SELECT 1 FROM temp_sql_scores s
            WHERE s.content = c.content
              AND ROUND(s.bm25_score::numeric, 4)
                  = ROUND((-c.score)::numeric, 4)
        )
    ) INTO v_idx_match_10
    FROM (SELECT row_number() OVER (ORDER BY score, content) AS rn,
                 content, score
          FROM temp_idx_cold_10) c
    FULL OUTER JOIN (SELECT row_number() OVER (ORDER BY score, content) AS rn,
                            content, score
                     FROM temp_idx_warm_10) w USING (rn);

    v_idx_sql := format($sql$
        SELECT
            %I as content,
            (%I <@> to_bm25query(%L, %L))::float8 as score
        FROM %I
        WHERE %I IS NOT NULL
        ORDER BY %I <@> to_bm25query(%L, %L)
        LIMIT %s
    $sql$, p_text_column, p_text_column, p_query, p_index_name,
          p_table_name, p_text_column,
          p_text_column, p_query, p_index_name, 100);

    EXECUTE format(
        'CREATE TEMP TABLE temp_idx_cold_100 ON COMMIT DROP AS %s', v_idx_sql);
    EXECUTE format(
        'CREATE TEMP TABLE temp_idx_warm_100 ON COMMIT DROP AS %s', v_idx_sql);

    SELECT bool_and(
        c.content IS NOT DISTINCT FROM w.content
        AND ROUND(c.score::numeric, 4)
            IS NOT DISTINCT FROM ROUND(w.score::numeric, 4)
        AND EXISTS (
            SELECT 1 FROM temp_sql_scores s
            WHERE s.content = c.content
              AND ROUND(s.bm25_score::numeric, 4)
                  = ROUND((-c.score)::numeric, 4)
        )
    ) INTO v_idx_match_100
    FROM (SELECT row_number() OVER (ORDER BY score, content) AS rn,
                 content, score
          FROM temp_idx_cold_100) c
    FULL OUTER JOIN (SELECT row_number() OVER (ORDER BY score, content) AS rn,
                            content, score
                     FROM temp_idx_warm_100) w USING (rn);

    -- Clean up
    DROP TABLE IF EXISTS temp_docs;
    DROP TABLE IF EXISTS temp_tf;
    DROP TABLE IF EXISTS temp_df;
    DROP TABLE IF EXISTS temp_query;
    DROP TABLE IF EXISTS temp_tapir_scores;
    DROP TABLE IF EXISTS temp_sql_scores;
    DROP TABLE IF EXISTS temp_idx_cold_10;
    DROP TABLE IF EXISTS temp_idx_warm_10;
    DROP TABLE IF EXISTS temp_idx_cold_100;
    DROP TABLE IF EXISTS temp_idx_warm_100;

    -- Restore caller's log_scores setting.  Skipped when v_saved_log_scores
    -- is NULL, which means we either didn't change the GUC (already 'off')
    -- or couldn't (non-superuser caller).
    IF v_saved_log_scores IS NOT NULL THEN
        BEGIN
            PERFORM set_config('pg_textsearch.log_scores',
                              v_saved_log_scores, true);
        EXCEPTION WHEN insufficient_privilege THEN
            -- Best-effort: caller lost privilege mid-call (unlikely)
            NULL;
        END;
    END IF;

    RETURN COALESCE(v_all_match, false)
        AND COALESCE(v_idx_match_10, true)
        AND COALESCE(v_idx_match_100, true);
END;
$$ LANGUAGE plpgsql;

-- Debug function to show detailed score computation
CREATE OR REPLACE FUNCTION debug_bm25_computation(
    p_table_name text,
    p_text_column text,
    p_index_name text,
    p_query text,
    p_text_config text,
    p_k1 float,
    p_b float
) RETURNS TABLE(
    doc_id integer,
    content text,
    doc_length integer,
    sql_bm25 float8,
    tapir_bm25 float8,
    difference float8,
    matches_4dp boolean,
    term_details jsonb
) AS $$
DECLARE
    v_total_docs integer;
    v_avg_doc_length float8;
    v_sql text;
BEGIN
    -- Tokenize documents with proper position counting
    EXECUTE format($sql$
        CREATE TEMP TABLE debug_docs ON COMMIT DROP AS
        WITH numbered AS (
            SELECT ROW_NUMBER() OVER () as doc_id, %I as content
            FROM %I WHERE %I IS NOT NULL
        ),
        doc_terms AS (
            SELECT
                doc_id, content, lexeme,
                array_length(string_to_array(positions::text, ','), 1) as term_count
            FROM numbered,
                 LATERAL unnest(to_tsvector(%L, content)) as t(lexeme, positions, weights)
        )
        SELECT doc_id, content,
               array_agg(DISTINCT lexeme) as unique_tokens,
               fieldnorm_quantize(SUM(term_count)::integer) as doc_length
        FROM doc_terms
        GROUP BY doc_id, content
    $sql$, p_text_column, p_table_name, p_text_column, p_text_config);

    -- Get statistics (using quantized doc lengths)
    SELECT COUNT(*), AVG(dd.doc_length)::float8
    INTO v_total_docs, v_avg_doc_length
    FROM debug_docs dd;

    -- Compute term frequencies with actual position counts
    EXECUTE format($sql$
        CREATE TEMP TABLE debug_tf ON COMMIT DROP AS
        WITH numbered AS (
            SELECT ROW_NUMBER() OVER () as doc_id, %I as content
            FROM %I WHERE %I IS NOT NULL
        )
        SELECT
            n.doc_id,
            t.lexeme as term,
            array_length(string_to_array(t.positions::text, ','), 1) as tf,
            (SELECT doc_length::int FROM debug_docs d WHERE d.doc_id = n.doc_id) as doc_len
        FROM numbered n,
             LATERAL unnest(to_tsvector(%L, n.content)) as t(lexeme, positions, weights)
    $sql$, p_text_column, p_table_name, p_text_column, p_text_config);

    CREATE TEMP TABLE debug_df ON COMMIT DROP AS
    SELECT term, COUNT(DISTINCT debug_tf.doc_id) as df
    FROM debug_tf GROUP BY term;

    -- Query terms
    EXECUTE format($sql$
        CREATE TEMP TABLE debug_query ON COMMIT DROP AS
        SELECT DISTINCT lexeme as term
        FROM unnest(to_tsvector(%L, %L))
    $sql$, p_text_config, p_query);

    -- Get Tapir scores
    v_sql := format($sql$
        CREATE TEMP TABLE debug_tapir_scores ON COMMIT DROP AS
        SELECT ROW_NUMBER() OVER () as doc_id,
               (%I <@> to_bm25query(%L, %L))::float8 as score
        FROM %I WHERE %I IS NOT NULL
        ORDER BY ROW_NUMBER() OVER ()
    $sql$, p_text_column, p_query, p_index_name, p_table_name, p_text_column);
    EXECUTE v_sql;

    -- Compute and return scores with details
    RETURN QUERY
    WITH score_details AS (
        SELECT
            d.doc_id,
            d.content::text,
            d.doc_length::int as doc_length,
            COALESCE(SUM(
                ln(1 + (v_total_docs - df.df + 0.5)::float8 / (df.df + 0.5)) *
                (tf.tf * (p_k1 + 1)) / (tf.tf + p_k1 * (1 - p_b + p_b * tf.doc_len / v_avg_doc_length))
            ), 0) as sql_score,
            jsonb_agg(
                CASE WHEN tf.tf IS NOT NULL THEN
                jsonb_build_object(
                    'term', q.term,
                    'tf', tf.tf,
                    'df', df.df,
                    'idf', ln(1 + (v_total_docs - df.df + 0.5)::float8 / (df.df + 0.5)),
                    'score', ln(1 + (v_total_docs - df.df + 0.5)::float8 / (df.df + 0.5)) *
                             (tf.tf * (p_k1 + 1)) / (tf.tf + p_k1 * (1 - p_b + p_b * tf.doc_len / v_avg_doc_length))
                )
                ELSE NULL END
            ) as term_details
        FROM debug_docs d
        CROSS JOIN debug_query q
        LEFT JOIN debug_tf tf ON tf.doc_id = d.doc_id AND tf.term = q.term
        LEFT JOIN debug_df df ON df.term = q.term
        GROUP BY d.doc_id, d.content, d.doc_length
    )
    SELECT
        s.doc_id::integer,
        s.content,
        s.doc_length::integer,
        s.sql_score,
        -t.score as tapir_score,
        ABS(s.sql_score - (-t.score)) as difference,
        ROUND(s.sql_score::numeric, 4) = ROUND((-t.score)::numeric, 4) as matches_4dp,
        s.term_details
    FROM score_details s
    JOIN debug_tapir_scores t ON s.doc_id = t.doc_id
    ORDER BY s.sql_score DESC;

    -- Clean up
    DROP TABLE IF EXISTS debug_docs;
    DROP TABLE IF EXISTS debug_tf;
    DROP TABLE IF EXISTS debug_df;
    DROP TABLE IF EXISTS debug_query;
    DROP TABLE IF EXISTS debug_tapir_scores;
END;
$$ LANGUAGE plpgsql;

-- Validate that index scan and standalone scoring produce identical results.
-- Index scan is triggered by ORDER BY score LIMIT; standalone runs on all
-- rows.  The index query is executed TWICE so that, when the memtable cache
-- is enabled, the first run exercises the cold/lazy-build path and the
-- second run exercises the warm path; both runs must agree with standalone
-- scoring and with each other.
CREATE OR REPLACE FUNCTION validate_index_vs_standalone(
    p_table_name text,
    p_text_column text,
    p_index_name text,
    p_query text,
    p_limit integer DEFAULT 100
) RETURNS boolean AS $$
DECLARE
    v_cold_match boolean;
    v_warm_match boolean;
    v_cold_eq_warm boolean;
    v_idx_sql text;
    v_sql text;
    v_saved_log_scores text;
BEGIN
    -- Suppress per-row BM25 NOTICEs from the helper's internal queries so
    -- output stays stable whether the caller has set log_scores or not, and
    -- so the cold+warm doubled execution does not duplicate NOTICE lines.
    -- log_scores is PGC_SUSET; skip the set_config when it's already off
    -- and tolerate insufficient_privilege for non-superuser callers.
    v_saved_log_scores := current_setting('pg_textsearch.log_scores', true);
    IF v_saved_log_scores IS DISTINCT FROM 'off' THEN
        BEGIN
            PERFORM set_config('pg_textsearch.log_scores', 'off', true);
        EXCEPTION WHEN insufficient_privilege THEN
            v_saved_log_scores := NULL;
        END;
    END IF;

    -- Index scan query reused for cold and warm runs
    v_idx_sql := format($sql$
        SELECT
            %I as content,
            (%I <@> to_bm25query(%L, %L))::float8 as score
        FROM %I
        WHERE %I IS NOT NULL
        ORDER BY %I <@> to_bm25query(%L, %L)
        LIMIT %s
    $sql$, p_text_column, p_text_column, p_query, p_index_name,
          p_table_name, p_text_column,
          p_text_column, p_query, p_index_name, p_limit);

    -- Cold run (first touch of the index after any prior state)
    EXECUTE format(
        'CREATE TEMP TABLE idx_scan_scores_cold ON COMMIT DROP AS %s',
        v_idx_sql);

    -- Warm run (re-execute identical query; exercises warm cache when enabled)
    EXECUTE format(
        'CREATE TEMP TABLE idx_scan_scores_warm ON COMMIT DROP AS %s',
        v_idx_sql);

    -- Get scores via standalone scoring mode (no ORDER BY score)
    v_sql := format($sql$
        CREATE TEMP TABLE standalone_scores ON COMMIT DROP AS
        SELECT
            %I as content,
            (%I <@> to_bm25query(%L, %L))::float8 as score
        FROM %I
        WHERE %I IS NOT NULL
          AND (%I <@> to_bm25query(%L, %L)) < 0
    $sql$, p_text_column, p_text_column, p_query, p_index_name,
          p_table_name, p_text_column,
          p_text_column, p_query, p_index_name);
    EXECUTE v_sql;

    -- Verify cold run matches standalone
    SELECT bool_and(
        EXISTS (
            SELECT 1 FROM standalone_scores s
            WHERE s.content = i.content
              AND ROUND(s.score::numeric, 6) = ROUND(i.score::numeric, 6)
        )
    ) INTO v_cold_match
    FROM idx_scan_scores_cold i;

    -- Verify warm run matches standalone
    SELECT bool_and(
        EXISTS (
            SELECT 1 FROM standalone_scores s
            WHERE s.content = i.content
              AND ROUND(s.score::numeric, 6) = ROUND(i.score::numeric, 6)
        )
    ) INTO v_warm_match
    FROM idx_scan_scores_warm i;

    -- Verify cold and warm produce identical ranked results.  Use
    -- IS NOT DISTINCT FROM so one-sided rows from FULL OUTER JOIN fail
    -- the comparison instead of being masked by NULL propagation, and
    -- give row_number() a deterministic ordering so tied scores rank
    -- the same in both subqueries.
    SELECT bool_and(
        c.content IS NOT DISTINCT FROM w.content
        AND ROUND(c.score::numeric, 6)
            IS NOT DISTINCT FROM ROUND(w.score::numeric, 6)
    ) INTO v_cold_eq_warm
    FROM (SELECT row_number() OVER (ORDER BY score, content) AS rn,
                 content, score
          FROM idx_scan_scores_cold) c
    FULL OUTER JOIN (SELECT row_number() OVER (ORDER BY score, content) AS rn,
                            content, score
                     FROM idx_scan_scores_warm) w USING (rn);

    -- Cleanup
    DROP TABLE IF EXISTS idx_scan_scores_cold;
    DROP TABLE IF EXISTS idx_scan_scores_warm;
    DROP TABLE IF EXISTS standalone_scores;

    -- Restore caller's log_scores setting.  Skipped when v_saved_log_scores
    -- is NULL (we didn't change it, or couldn't as a non-superuser).
    IF v_saved_log_scores IS NOT NULL THEN
        BEGIN
            PERFORM set_config('pg_textsearch.log_scores',
                              v_saved_log_scores, true);
        EXCEPTION WHEN insufficient_privilege THEN
            NULL;
        END;
    END IF;

    RETURN COALESCE(v_cold_match, true)
        AND COALESCE(v_warm_match, true)
        AND COALESCE(v_cold_eq_warm, true);
END;
$$ LANGUAGE plpgsql;

-- Simpler comparison function
CREATE OR REPLACE FUNCTION compare_bm25_scores(
    p_table_name text,
    p_text_column text,
    p_index_name text,
    p_query text,
    p_text_config text,
    p_k1 float,
    p_b float
) RETURNS TABLE(
    doc_id integer,
    content text,
    sql_score float8,
    tapir_score float8,
    difference float8,
    matches_4dp boolean
) AS $$
DECLARE
    v_total_docs integer;
    v_avg_doc_length float8;
    v_sql text;
BEGIN
    -- Tokenize documents with position counting
    EXECUTE format($sql$
        CREATE TEMP TABLE comp_docs ON COMMIT DROP AS
        WITH numbered AS (
            SELECT ROW_NUMBER() OVER () as doc_id, %I as content
            FROM %I WHERE %I IS NOT NULL
        ),
        doc_terms AS (
            SELECT doc_id, content, lexeme,
                   array_length(string_to_array(positions::text, ','), 1) as term_count
            FROM numbered,
                 LATERAL unnest(to_tsvector(%L, content)) as t(lexeme, positions, weights)
        )
        SELECT doc_id, content,
               fieldnorm_quantize(SUM(term_count)::integer) as doc_length
        FROM doc_terms
        GROUP BY doc_id, content
    $sql$, p_text_column, p_table_name, p_text_column, p_text_config);

    -- Get statistics
    SELECT COUNT(*), AVG(c.doc_length)::float8
    INTO v_total_docs, v_avg_doc_length
    FROM comp_docs c;

    -- Build term frequencies with position counts
    EXECUTE format($sql$
        CREATE TEMP TABLE comp_tf ON COMMIT DROP AS
        WITH numbered AS (
            SELECT ROW_NUMBER() OVER () as doc_id, %I as content
            FROM %I WHERE %I IS NOT NULL
        )
        SELECT
            n.doc_id,
            t.lexeme as term,
            array_length(string_to_array(t.positions::text, ','), 1) as tf,
            (SELECT doc_length::int FROM comp_docs c WHERE c.doc_id = n.doc_id) as doc_len
        FROM numbered n,
             LATERAL unnest(to_tsvector(%L, n.content)) as t(lexeme, positions, weights)
    $sql$, p_text_column, p_table_name, p_text_column, p_text_config);

    -- Document frequencies
    CREATE TEMP TABLE comp_df ON COMMIT DROP AS
    SELECT term, COUNT(DISTINCT comp_tf.doc_id) as df
    FROM comp_tf GROUP BY term;

    -- Query terms
    EXECUTE format($sql$
        CREATE TEMP TABLE comp_query ON COMMIT DROP AS
        SELECT DISTINCT lexeme as term
        FROM unnest(to_tsvector(%L, %L))
    $sql$, p_text_config, p_query);

    -- Tapir scores
    v_sql := format($sql$
        CREATE TEMP TABLE comp_tapir ON COMMIT DROP AS
        SELECT ROW_NUMBER() OVER () as doc_id,
               (%I <@> to_bm25query(%L, %L))::float8 as score
        FROM %I WHERE %I IS NOT NULL
        ORDER BY ROW_NUMBER() OVER ()
    $sql$, p_text_column, p_query, p_index_name, p_table_name, p_text_column);
    EXECUTE v_sql;

    -- Compute and compare
    RETURN QUERY
    WITH sql_scores AS (
        SELECT
            d.doc_id,
            d.content::text,
            COALESCE(SUM(
                ln(1 + (v_total_docs - df.df + 0.5)::float8 / (df.df + 0.5)) *
                (tf.tf * (p_k1 + 1)) / (tf.tf + p_k1 * (1 - p_b + p_b * tf.doc_len / v_avg_doc_length))
            ), 0) as bm25_score
        FROM comp_docs d
        CROSS JOIN comp_query q
        LEFT JOIN comp_tf tf ON tf.doc_id = d.doc_id AND tf.term = q.term
        LEFT JOIN comp_df df ON df.term = q.term
        GROUP BY d.doc_id, d.content
    )
    SELECT
        s.doc_id::integer,
        s.content,
        s.bm25_score as sql_score,
        -t.score as tapir_score,
        ABS(s.bm25_score - (-t.score)) as difference,
        ROUND(s.bm25_score::numeric, 4) = ROUND((-t.score)::numeric, 4) as matches_4dp
    FROM sql_scores s
    JOIN comp_tapir t ON s.doc_id = t.doc_id
    ORDER BY s.bm25_score DESC;

    -- Clean up
    DROP TABLE IF EXISTS comp_docs;
    DROP TABLE IF EXISTS comp_tf;
    DROP TABLE IF EXISTS comp_df;
    DROP TABLE IF EXISTS comp_query;
    DROP TABLE IF EXISTS comp_tapir;
END;
$$ LANGUAGE plpgsql;
