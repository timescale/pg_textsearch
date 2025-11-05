#!/usr/bin/env python3
"""
Validate pg_textsearch BM25 implementation against reference Python implementation
"""

import math
import psycopg2
import sys
from collections import Counter
import re


class BM25:
    """Reference BM25 implementation for validation"""

    def __init__(self, k1=1.2, b=0.75):
        self.k1 = k1
        self.b = b
        self.doc_freqs = {}
        self.doc_lengths = {}
        self.avg_doc_length = 0
        self.total_docs = 0
        self.documents = {}

    def tokenize(self, text):
        """Simple tokenizer matching PostgreSQL's english text search config"""
        # Convert to lowercase and split on non-alphanumeric
        text = text.lower()
        tokens = re.findall(r'\b[a-z]+\b', text)
        # Basic stemming - very simplified, just for common cases
        # PostgreSQL's english config uses Snowball stemmer
        stemmed = []
        for token in tokens:
            # Remove common endings (simplified)
            if token.endswith('ing') and len(token) > 5:
                token = token[:-3]
            elif token.endswith('ed') and len(token) > 4:
                token = token[:-2]
            elif token.endswith('s') and len(token) > 3 and not token.endswith('ss'):
                token = token[:-1]
            stemmed.append(token)
        return stemmed

    def add_document(self, doc_id, text):
        """Add a document to the index"""
        tokens = self.tokenize(text)
        self.documents[doc_id] = text
        self.doc_lengths[doc_id] = len(tokens)

        # Count term frequencies
        term_freqs = Counter(tokens)
        for term in term_freqs:
            if term not in self.doc_freqs:
                self.doc_freqs[term] = set()
            self.doc_freqs[term].add(doc_id)

        self.total_docs += 1
        self.avg_doc_length = sum(self.doc_lengths.values()) / self.total_docs

    def score(self, doc_id, query):
        """Calculate BM25 score for a document given a query"""
        query_terms = self.tokenize(query)
        doc_text = self.documents.get(doc_id, "")
        doc_tokens = self.tokenize(doc_text)
        doc_term_freqs = Counter(doc_tokens)

        score = 0.0
        doc_len = self.doc_lengths.get(doc_id, 0)

        for term in query_terms:
            if term not in doc_term_freqs:
                continue

            # Term frequency in document
            tf = doc_term_freqs[term]

            # Document frequency
            df = len(self.doc_freqs.get(term, set()))

            # IDF calculation: log((N - df + 0.5) / (df + 0.5))
            idf = math.log((self.total_docs - df + 0.5) / (df + 0.5))

            # BM25 formula
            numerator = tf * (self.k1 + 1)
            denominator = tf + self.k1 * (1 - self.b + self.b * doc_len / self.avg_doc_length)
            score += idf * (numerator / denominator)

        # Return negative score to match PostgreSQL's ordering convention
        return -score


def validate_scores(num_docs=100):
    """Validate pg_textsearch BM25 scores against Python implementation"""

    print("BM25 Score Validation")
    print("=" * 50)

    # Connect to PostgreSQL
    try:
        conn = psycopg2.connect(
            host="localhost",
            database="postgres",
            user="tjg"
        )
        cur = conn.cursor()
    except Exception as e:
        print(f"Failed to connect to PostgreSQL: {e}")
        return

    # Create test data
    print(f"\n1. Creating test data with {num_docs} documents...")
    cur.execute("DROP TABLE IF EXISTS bm25_validation CASCADE")
    cur.execute("""
        CREATE TABLE bm25_validation (
            id SERIAL PRIMARY KEY,
            content TEXT
        )
    """)

    # Generate test documents
    bm25 = BM25(k1=1.2, b=0.75)
    test_docs = []

    for i in range(1, num_docs + 1):
        if i % 5 == 1:
            content = f"The quick brown fox jumps over the lazy dog. Research shows interesting results."
        elif i % 5 == 2:
            content = f"Research in computer science involves algorithms and data structures."
        elif i % 5 == 3:
            content = f"Machine learning research has advanced significantly in recent years."
        elif i % 5 == 4:
            content = f"Database systems provide efficient data storage and retrieval."
        else:
            content = f"Software engineering practices improve code quality and maintainability."

        # Add some variation
        if i % 10 == 0:
            content += " Research research research."  # Multiple occurrences

        test_docs.append((i, content))
        bm25.add_document(i, content)

    # Insert into PostgreSQL
    cur.executemany("INSERT INTO bm25_validation (id, content) VALUES (%s, %s)", test_docs)

    # Create BM25 index
    print("2. Creating BM25 index...")
    cur.execute("""
        CREATE INDEX idx_bm25_val
        ON bm25_validation
        USING bm25 (content)
        WITH (text_config = 'english', k1 = 1.2, b = 0.75)
    """)

    conn.commit()

    # Test queries
    test_queries = ["research", "computer science", "machine learning"]

    print("\n3. Comparing scores for test queries...")
    print("-" * 50)

    for query in test_queries:
        print(f"\nQuery: '{query}'")
        print("-" * 30)

        # Get PostgreSQL scores
        cur.execute("""
            SELECT id, content,
                   CAST(content <@> to_bm25query(%s, 'idx_bm25_val') AS float) as pg_score
            FROM bm25_validation
            WHERE content @@ to_tsquery('english', %s)
            ORDER BY content <@> to_bm25query(%s, 'idx_bm25_val')
            LIMIT 5
        """, (query, query.replace(' ', ' & '), query))

        results = cur.fetchall()

        if not results:
            print("  No results found")
            continue

        # Compare scores
        print(f"{'ID':>4} | {'PG Score':>10} | {'Py Score':>10} | {'Diff':>8} | Content Preview")
        print("-" * 70)

        for doc_id, content, pg_score in results:
            py_score = bm25.score(doc_id, query)
            diff = abs(pg_score - py_score)
            content_preview = content[:30] + "..."

            # Flag large differences
            flag = " ⚠️" if diff > 0.1 else ""

            print(f"{doc_id:4d} | {pg_score:10.4f} | {py_score:10.4f} | {diff:8.4f}{flag} | {content_preview}")

    # Statistical comparison
    print("\n4. Statistical Comparison")
    print("-" * 50)

    # Get all scores for a comprehensive comparison
    query = "research"
    cur.execute("""
        SELECT id,
               CAST(content <@> to_bm25query(%s, 'idx_bm25_val') AS float) as pg_score
        FROM bm25_validation
        WHERE content @@ to_tsquery('english', %s)
    """, (query, query))

    all_results = cur.fetchall()

    if all_results:
        pg_scores = [r[1] for r in all_results]
        py_scores = [bm25.score(r[0], query) for r in all_results]

        # Calculate correlation
        from statistics import mean, stdev
        if len(pg_scores) > 1:
            pg_mean = mean(pg_scores)
            py_mean = mean(py_scores)
            pg_std = stdev(pg_scores) if len(pg_scores) > 1 else 1
            py_std = stdev(py_scores) if len(py_scores) > 1 else 1

            # Pearson correlation
            if pg_std > 0 and py_std > 0:
                correlation = sum((pg - pg_mean) * (py - py_mean)
                                  for pg, py in zip(pg_scores, py_scores))
                correlation /= (len(pg_scores) - 1) * pg_std * py_std
                print(f"Correlation coefficient: {correlation:.4f}")

        # Average absolute difference
        avg_diff = mean(abs(pg - py) for pg, py in zip(pg_scores, py_scores))
        print(f"Average absolute difference: {avg_diff:.6f}")
        max_diff = max(abs(pg - py) for pg, py in zip(pg_scores, py_scores))
        print(f"Maximum absolute difference: {max_diff:.6f}")

        if avg_diff < 0.01:
            print("\n✅ Scores are very close - implementation appears correct!")
        elif avg_diff < 0.1:
            print("\n⚠️  Small differences detected - likely due to tokenization differences")
        else:
            print("\n❌ Large differences detected - review implementation")

    # Cleanup
    cur.execute("DROP TABLE bm25_validation CASCADE")
    conn.commit()
    cur.close()
    conn.close()

    print("\nValidation complete!")


if __name__ == "__main__":
    num_docs = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    validate_scores(num_docs)
