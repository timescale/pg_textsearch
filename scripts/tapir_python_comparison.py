#!/usr/bin/env python3
"""
Compare Tapir BM25 scoring with Python implementation using identical corpus
"""

import math
import psycopg2
from psycopg2.extras import RealDictCursor
from rank_bm25 import BM25Okapi

def compare_tapir_python():
    # Connect to testdb (same as Tapir)
    conn = psycopg2.connect(dbname='testdb')
    
    # Fetch the exact same corpus Tapir is using
    with conn.cursor(cursor_factory=RealDictCursor) as cur:
        query = """
            SELECT 
                id,
                content,
                to_tsvector('english', content)::text as tsvector_text
            FROM aerodocs_final
            ORDER BY id
        """
        cur.execute(query)
        results = cur.fetchall()
    
    print("=== CORPUS COMPARISON ===")
    corpus = []
    doc_ids = []
    
    for row in results:
        doc_ids.append(row['id'])
        
        # Extract stemmed terms from tsvector (same as Tapir processing)
        tsvector_str = row['tsvector_text']
        # Parse tsvector format: 'term1':pos1 'term2':pos2 ...
        terms = []
        parts = tsvector_str.split("'")
        for i in range(1, len(parts), 2):
            if i < len(parts):
                term = parts[i]
                terms.append(term)
        
        corpus.append(terms)
        print(f"Doc {row['id']}: {row['content']}")
        print(f"  Terms: {terms}")
        print()
    
    # Initialize BM25 with same parameters as Tapir
    bm25 = BM25Okapi(corpus, k1=1.2, b=0.75)
    
    print("=== PYTHON BM25 STATISTICS ===")
    print(f"Python BM25 initialized: corpus_size={len(corpus)}, avgdl={bm25.avgdl}")
    print(f"Python average_idf={bm25.average_idf}")
    print()
    
    # Process query through to_tsvector just like Tapir does
    with conn.cursor(cursor_factory=RealDictCursor) as cur:
        cur.execute("SELECT to_tsvector('english', %s)::text as tsvector_text", ('aerodynamic analysis',))
        query_result = cur.fetchone()
    
    # Extract query terms and frequencies from tsvector
    query_tsvector_str = query_result['tsvector_text']
    print(f"Query tsvector: {query_tsvector_str}")
    
    # Parse tsvector format: 'term1':pos1 'term2':pos2 ...
    query_terms = []
    query_parts = query_tsvector_str.split("'")
    for i in range(1, len(query_parts), 2):
        if i < len(query_parts):
            term = query_parts[i]
            query_terms.append(term)
    
    print(f"Processed query terms: {query_terms}")
    print(f"Query terms: {query_terms}")
    print()
    
    print("=== PYTHON IDF CALCULATION TRACING ===")
    print()
    
    for term in query_terms:
        print(f"--- Term: '{term}' ---")
        
        # Calculate document frequency
        doc_freq = sum(1 for doc in corpus if term in doc)
        total_docs = len(corpus)
        
        # Calculate IDF manually to match Tapir's calculation
        idf_numerator = total_docs - doc_freq + 0.5
        idf_denominator = doc_freq + 0.5
        idf_ratio = idf_numerator / idf_denominator
        raw_idf = math.log(idf_ratio)
        
        print(f"PYTHON IDF: doc_freq={doc_freq}, total_docs={total_docs}, average_idf={bm25.average_idf:.6f}")
        print(f"PYTHON IDF: numerator={idf_numerator:.6f}, denominator={idf_denominator:.6f}, ratio={idf_ratio:.6f}, raw_idf={raw_idf:.6f}")
        
        # Check if epsilon should be applied (Python BM25 behavior)
        if raw_idf < 0.0:
            expected_epsilon = 0.25 * bm25.average_idf
            print(f"PYTHON IDF: EPSILON APPLIED (raw_idf < 0.0), expected_epsilon={expected_epsilon:.6f}")
            actual_idf = bm25.idf[term] if term in bm25.idf else 0.0
            print(f"PYTHON IDF: Python BM25 actual IDF={actual_idf:.6f}")
            print(f"PYTHON IDF: Epsilon match: {abs(actual_idf - expected_epsilon) < 0.0001}")
        else:
            print(f"PYTHON IDF: NO EPSILON (raw_idf >= 0.0), Python BM25 actual IDF={raw_idf:.6f}")
            actual_idf = bm25.idf[term] if term in bm25.idf else raw_idf
            print(f"PYTHON IDF: Raw IDF match: {abs(actual_idf - raw_idf) < 0.0001}")
        print()
    
    # Calculate full BM25 scores for comparison
    print("=== FULL BM25 SCORE COMPARISON ===")
    query = ['aerodynam', 'analysi']  # Same as Tapir query
    scores = bm25.get_scores(query)
    
    for i, score in enumerate(scores):
        print(f"Doc {doc_ids[i]}: Python BM25 score = {score:.10f}")
    
    print()
    print("Compare with Tapir score (should be negative of Python positive scores):")
    print("Tapir uses negative scores for ASC ordering in PostgreSQL")

if __name__ == "__main__":
    compare_tapir_python()