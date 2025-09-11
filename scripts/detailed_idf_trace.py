#!/usr/bin/env python3
"""
Detailed IDF calculation tracing to match Tapir's debug output
"""

import math
import psycopg2
from psycopg2.extras import RealDictCursor
from rank_bm25 import BM25Okapi

def trace_python_idf():
    # Connect to database
    conn = psycopg2.connect(dbname='contrib_regression')
    
    # Fetch corpus exactly as validation script does
    with conn.cursor(cursor_factory=RealDictCursor) as cur:
        query = """
            SELECT 
                doc_id,
                full_text as original_text,
                to_tsvector('english', full_text)::text as tsvector_text
            FROM aerodocs_documents
            ORDER BY doc_id
        """
        cur.execute(query)
        results = cur.fetchall()
    
    corpus = []
    doc_ids = []
    
    print("=== PYTHON CORPUS PROCESSING ===")
    for row in results:
        tsvector_text = row['tsvector_text']
        terms = []
        
        # Parse tsvector format: 'term1:pos1,pos2 term2:pos3'
        for token in tsvector_text.split():
            if ':' in token:
                term = token.split(':')[0].strip("'")
                positions = token.split(':')[1].split(',')
                terms.extend([term] * len(positions))
            else:
                terms.append(token.strip("'"))
        
        corpus.append(terms)
        doc_ids.append(row['doc_id'])
    
    # Initialize BM25
    k1, b = 1.2, 0.75
    bm25 = BM25Okapi(corpus, k1=k1, b=b)
    
    print(f"Python BM25 initialized: corpus_size={bm25.corpus_size}, avgdl={bm25.avgdl}")
    print(f"Python average_idf={bm25.average_idf}")
    
    # Process the same query
    test_query = "aerodynamic flow analysis"
    with conn.cursor() as cur:
        cur.execute("SELECT to_tsvector('english', %s)::text", (test_query,))
        tsvector_result = cur.fetchone()[0]
    
    query_terms = []
    for token in tsvector_result.split():
        if ':' in token:
            term = token.split(':')[0].strip("'")
            query_terms.append(term)
        else:
            query_terms.append(token.strip("'"))
    
    print(f"\nQuery terms: {query_terms}")
    
    # Trace IDF calculation for each query term
    print(f"\n=== PYTHON IDF CALCULATION TRACING ===")
    
    for term in query_terms:
        print(f"\n--- Term: '{term}' ---")
        
        # Calculate document frequency manually
        doc_freq = sum(1 for doc_freqs in bm25.doc_freqs if term in doc_freqs)
        total_docs = bm25.corpus_size
        
        print(f"PYTHON IDF: doc_freq={doc_freq}, total_docs={total_docs}, average_idf={bm25.average_idf:.6f}")
        
        # Manual IDF calculation matching Tapir's formula
        idf_numerator = total_docs - doc_freq + 0.5
        idf_denominator = doc_freq + 0.5
        idf_ratio = idf_numerator / idf_denominator
        raw_idf = math.log(idf_ratio)
        
        print(f"PYTHON IDF: numerator={idf_numerator:.6f}, denominator={idf_denominator:.6f}, ratio={idf_ratio:.6f}, raw_idf={raw_idf:.6f}")
        
        # Check Python BM25's actual IDF value
        python_idf = bm25.idf.get(term, 0)
        
        if raw_idf < 0:
            expected_epsilon = 0.25 * bm25.average_idf
            print(f"PYTHON IDF: EPSILON APPLIED (raw_idf < 0.0), expected_epsilon={expected_epsilon:.6f}")
            print(f"PYTHON IDF: Python BM25 actual IDF={python_idf:.6f}")
            print(f"PYTHON IDF: Epsilon match: {abs(python_idf - expected_epsilon) < 0.000001}")
        else:
            print(f"PYTHON IDF: NO EPSILON (raw_idf >= 0.0), Python BM25 actual IDF={python_idf:.6f}")
            print(f"PYTHON IDF: Raw IDF match: {abs(python_idf - raw_idf) < 0.000001}")
    
    conn.close()

if __name__ == "__main__":
    trace_python_idf()