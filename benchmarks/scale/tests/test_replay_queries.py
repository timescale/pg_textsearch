import os
import unittest

from benchmarks.scale.replay_queries import (
    prepared_statement,
    replay_case,
    select_queries,
    validate_ranked_plan,
)


class ReplayTests(unittest.TestCase):
    def test_selection_preserves_order_without_duplicates(self):
        queries = [{"id": str(n)} for n in range(4)]
        self.assertEqual(
            [ordinal for ordinal, _ in select_queries(queries, [-1, 0, -1])],
            [3, 0],
        )
        self.assertEqual(
            [ordinal for ordinal, _ in select_queries(queries)], [0, 3]
        )
        with self.assertRaises(ValueError):
            select_queries(queries, [-5])
        with self.assertRaises(ValueError):
            select_queries([], None)

    def test_ranked_sql_binds_filter_and_scoring_text_independently(self):
        self.assertEqual(
            prepared_statement("search", 10),
            "SELECT id FROM documents WHERE body @@ %(tsquery)s::tsquery "
            "ORDER BY body <@> "
            "to_bm25query(%(text)s, 'documents_bm25') LIMIT 10",
        )
        self.assertEqual(
            prepared_statement("count", 10),
            "SELECT count(*) FROM documents "
            "WHERE numnode(%(tsquery)s::tsquery) > 0 "
            "AND body @@ %(tsquery)s::tsquery",
        )

    def test_gate_rejects_materialized_and_executor_filtered_plans(self):
        scan = {
            "Node Type": "Index Scan", "Index Name": "documents_bm25",
            "Index Cond": "body @@ query", "Order By": "body <@> query",
        }
        self.assertTrue(validate_ranked_plan(
            [{"Plan": {"Node Type": "Limit", "Plans": [scan]}}]
        ))
        self.assertFalse(validate_ranked_plan([{"Plan": {
            "Node Type": "Limit", "Plans": [
                {"Node Type": "Sort", "Plans": [scan]},
            ],
        }}]))
        self.assertFalse(validate_ranked_plan([{"Plan": {
            "Node Type": "Index Scan", "Index Name": "documents_bm25",
            "Order By": "body <@> query", "Filter": "body @@ query",
        }}]))


@unittest.skipUnless(
    os.environ.get("PGTS_REPLAY_TEST_DSN"),
    "requires an isolated PostgreSQL server with ranked Boolean pushdown",
)
class ReplayIntegrationTests(unittest.TestCase):
    def test_real_replay_and_timeout_recovery(self):
        import psycopg

        with psycopg.connect(
            os.environ["PGTS_REPLAY_TEST_DSN"], autocommit=True
        ) as connection:
            connection.execute("SET default_text_search_config='simple'")
            connection.execute("SET enable_seqscan=off")
            connection.execute(
                "CREATE TEMP TABLE documents"
                "(id integer PRIMARY KEY, body text)"
            )
            connection.execute(
                "INSERT INTO documents VALUES "
                "(1, 'alpha alpha alpha'), (2, 'alpha beta'), "
                "(3, 'alpha alpha beta')"
            )
            connection.execute(
                "CREATE INDEX documents_bm25 ON documents USING bm25(body) "
                "WITH (text_config='simple')"
            )
            query = {
                "id": "fixture", "text": "alpha",
                "pg_tsquery": {"conjunction": "'beta'"},
            }
            result = replay_case(
                connection, query, "conjunction", "search", 1, 2
            )
            self.assertEqual(result["status"], "ok")
            self.assertEqual(result["rows"], [[3]])
            self.assertTrue(result["row_filter_verified"])
            self.assertTrue(result["ranked_plan_verified"])
            connection.execute("SET statement_timeout='1ms'")
            with self.assertRaises(psycopg.errors.QueryCanceled):
                connection.execute("SELECT pg_sleep(0.1)")
            connection.execute("SET statement_timeout=0")
            self.assertEqual(connection.execute("SELECT 1").fetchone(), (1,))


if __name__ == "__main__":
    unittest.main()
