-- BM25 Score Validation Functions
--
-- Formula: BM25(q,d) = Σ IDF(qi) * (tf(qi,d) * (k1 + 1)) / (tf(qi,d) + k1 * (1 - b + b * |d| / avgdl))
-- Where:
--   IDF(qi) = ln(1 + (N - df(qi) + 0.5) / (df(qi) + 0.5))
--   tf(qi,d) = term frequency of qi in document d
--   |d| = document length, avgdl = average document length
--   k1 = term frequency saturation, b = length normalization
--
-- Usage Example:
--   SELECT validate_bm25_scoring('mytable', 'content_column', 'my_bm25_idx', 'search query', 'english', 1.2, 0.75)

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
    v_sql text;
BEGIN
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
                 LATERAL unnest(to_tsvector('%s', content)) as t(lexeme, positions, weights)
        )
        SELECT
            doc_id,
            content,
            array_agg(lexeme ORDER BY lexeme) as unique_tokens,
            SUM(term_count) as doc_length
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
             LATERAL unnest(to_tsvector('%s', n.content)) as t(lexeme, positions, weights)
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
        FROM unnest(to_tsvector('%s', %L))
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

    -- Step 7: Compute BM25 scores and compare to 4 decimal places
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
    SELECT bool_and(
        -- Compare to 4 decimal places
        ROUND(s.bm25_score::numeric, 4) = ROUND((-t.score)::numeric, 4)
    ) INTO v_all_match
    FROM sql_scores s
    JOIN temp_tapir_scores t ON s.doc_id = t.doc_id;

    -- Clean up
    DROP TABLE IF EXISTS temp_docs;
    DROP TABLE IF EXISTS temp_tf;
    DROP TABLE IF EXISTS temp_df;
    DROP TABLE IF EXISTS temp_query;
    DROP TABLE IF EXISTS temp_tapir_scores;

    RETURN COALESCE(v_all_match, false);
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
                 LATERAL unnest(to_tsvector('%s', content)) as t(lexeme, positions, weights)
        )
        SELECT doc_id, content,
               array_agg(DISTINCT lexeme) as unique_tokens,
               SUM(term_count) as doc_length
        FROM doc_terms
        GROUP BY doc_id, content
    $sql$, p_text_column, p_table_name, p_text_column, p_text_config);

    -- Get statistics
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
             LATERAL unnest(to_tsvector('%s', n.content)) as t(lexeme, positions, weights)
    $sql$, p_text_column, p_table_name, p_text_column, p_text_config);

    CREATE TEMP TABLE debug_df ON COMMIT DROP AS
    SELECT term, COUNT(DISTINCT debug_tf.doc_id) as df
    FROM debug_tf GROUP BY term;

    -- Query terms
    EXECUTE format($sql$
        CREATE TEMP TABLE debug_query ON COMMIT DROP AS
        SELECT DISTINCT lexeme as term
        FROM unnest(to_tsvector('%s', %L))
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

-- Validate that index scan and standalone scoring produce identical results
-- Index scan is triggered by ORDER BY score LIMIT; standalone runs on all rows
CREATE OR REPLACE FUNCTION validate_index_vs_standalone(
    p_table_name text,
    p_text_column text,
    p_index_name text,
    p_query text,
    p_limit integer DEFAULT 100
) RETURNS boolean AS $$
DECLARE
    v_all_match boolean;
    v_sql text;
BEGIN
    -- Get scores via index scan mode (ORDER BY score triggers index use)
    v_sql := format($sql$
        CREATE TEMP TABLE idx_scan_scores ON COMMIT DROP AS
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
    EXECUTE v_sql;

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

    -- Verify all index scan results exist in standalone with matching scores
    SELECT bool_and(
        EXISTS (
            SELECT 1 FROM standalone_scores s
            WHERE s.content = i.content
              AND ROUND(s.score::numeric, 6) = ROUND(i.score::numeric, 6)
        )
    ) INTO v_all_match
    FROM idx_scan_scores i;

    -- Cleanup
    DROP TABLE IF EXISTS idx_scan_scores;
    DROP TABLE IF EXISTS standalone_scores;

    RETURN COALESCE(v_all_match, true);
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
                 LATERAL unnest(to_tsvector('%s', content)) as t(lexeme, positions, weights)
        )
        SELECT doc_id, content,
               SUM(term_count) as doc_length
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
             LATERAL unnest(to_tsvector('%s', n.content)) as t(lexeme, positions, weights)
    $sql$, p_text_column, p_table_name, p_text_column, p_text_config);

    -- Document frequencies
    CREATE TEMP TABLE comp_df ON COMMIT DROP AS
    SELECT term, COUNT(DISTINCT comp_tf.doc_id) as df
    FROM comp_tf GROUP BY term;

    -- Query terms
    EXECUTE format($sql$
        CREATE TEMP TABLE comp_query ON COMMIT DROP AS
        SELECT DISTINCT lexeme as term
        FROM unnest(to_tsvector('%s', %L))
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
