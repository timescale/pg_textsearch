#!/usr/bin/env python3
"""
Detailed BM25 calculation breakdown to find exact divergence point
"""

import math
import psycopg2
from psycopg2.extras import RealDictCursor
from rank_bm25 import BM25Okapi
import numpy as np

def detailed_bm25_debug():
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
    
    print("=== CORPUS PARSING ===")
    for row in results:
        tsvector_text = row['tsvector_text']
        terms = []
        
        print(f"Doc {row['doc_id']}: '{row['original_text']}'")
        print(f"  tsvector: {tsvector_text}")
        
        # Parse tsvector format: 'term1:pos1,pos2 term2:pos3'
        for token in tsvector_text.split():
            if ':' in token:
                term = token.split(':')[0].strip("'")
                positions = token.split(':')[1].split(',')
                terms.extend([term] * len(positions))
            else:
                terms.append(token.strip("'"))
        
        print(f"  parsed terms: {terms}")
        print(f"  term frequencies: {dict((t, terms.count(t)) for t in set(terms))}")
        
        corpus.append(terms)
        doc_ids.append(row['doc_id'])
    
    # Initialize BM25
    k1, b = 1.2, 0.75
    bm25 = BM25Okapi(corpus, k1=k1, b=b)
    
    print(f"\n=== PYTHON BM25 PARAMETERS ===")
    print(f"k1 = {k1}, b = {b}")
    print(f"corpus_size = {bm25.corpus_size}")
    print(f"avgdl = {bm25.avgdl}")
    print(f"doc_len = {bm25.doc_len}")
    print(f"epsilon = {bm25.epsilon}")
    print(f"average_idf = {bm25.average_idf}")
    
    # Process query
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
    
    print(f"\n=== QUERY PROCESSING ===")
    print(f"Original query: '{test_query}'")
    print(f"Query tsvector: {tsvector_result}")
    print(f"Parsed query terms: {query_terms}")
    
    # Calculate Python BM25 scores
    python_scores = bm25.get_scores(query_terms)
    
    print(f"\n=== PYTHON BM25 IDF CALCULATIONS ===")
    for term in query_terms:
        if term in bm25.idf:
            # Calculate manually
            term_doc_freq = sum(1 for doc_freqs in bm25.doc_freqs if term in doc_freqs)
            manual_idf = math.log(bm25.corpus_size - term_doc_freq + 0.5) - math.log(term_doc_freq + 0.5)
            
            print(f"Term '{term}':")
            print(f"  Document frequency: {term_doc_freq} out of {bm25.corpus_size} docs")
            print(f"  Manual IDF calculation: log({bm25.corpus_size} - {term_doc_freq} + 0.5) - log({term_doc_freq} + 0.5)")
            print(f"    = log({bm25.corpus_size - term_doc_freq + 0.5}) - log({term_doc_freq + 0.5})")  
            print(f"    = {math.log(bm25.corpus_size - term_doc_freq + 0.5)} - {math.log(term_doc_freq + 0.5)}")
            print(f"    = {manual_idf}")
            print(f"  Python BM25 IDF: {bm25.idf[term]}")
            print(f"  Match: {abs(manual_idf - bm25.idf[term]) < 0.000001}")
    
    print(f"\n=== PYTHON BM25 SCORING BREAKDOWN ===")
    for doc_idx, doc_id in enumerate(doc_ids):
        print(f"\nDoc {doc_id} (Python calculation):")
        doc_len = bm25.doc_len[doc_idx]
        doc_freqs = bm25.doc_freqs[doc_idx]
        print(f"  Document length: {doc_len}")
        print(f"  Document term frequencies: {doc_freqs}")
        print(f"  Length normalization: doc_len/avgdl = {doc_len}/{bm25.avgdl} = {doc_len/bm25.avgdl}")
        
        total_score = 0.0
        for term in query_terms:
            tf = doc_freqs.get(term, 0)
            idf = bm25.idf.get(term, 0)
            
            numerator = tf * (k1 + 1)
            denominator = tf + k1 * (1 - b + b * doc_len / bm25.avgdl)
            term_score = idf * (numerator / denominator)
            total_score += term_score
            
            print(f"    Term '{term}': tf={tf}, idf={idf:.6f}")
            print(f"      numerator = {tf} * ({k1} + 1) = {numerator}")
            print(f"      denominator = {tf} + {k1} * (1 - {b} + {b} * {doc_len} / {bm25.avgdl}) = {denominator:.6f}")
            print(f"      term_score = {idf:.6f} * ({numerator} / {denominator:.6f}) = {term_score:.6f}")
        
        print(f"    Total Python score: {total_score:.6f}")
        print(f"    Python BM25 library score: {python_scores[doc_idx]:.6f}")
        print(f"    Manual vs Library match: {abs(total_score - python_scores[doc_idx]) < 0.000001}")
    
    # Now get Tapir scores with detailed debug
    print(f"\n=== TAPIR SCORING COMPARISON ===")
    with conn.cursor(cursor_factory=RealDictCursor) as cur:
        # Enable debug output
        cur.execute("SET client_min_messages = DEBUG1")
        
        query_sql = """
            SELECT doc_id, (full_text <@> to_tpvector(%s, %s)) as score
            FROM aerodocs_documents
            ORDER BY doc_id
        """
        cur.execute(query_sql, (test_query, 'cranfield_tapir_idx'))
        tapir_results = cur.fetchall()
    
    print(f"\n=== FINAL COMPARISON ===")
    print("Doc ID | Python Score | Tapir Score  | Difference   | Status")
    print("-------|--------------|--------------|--------------|--------")
    
    for i, doc_id in enumerate(doc_ids):
        tapir_score = tapir_results[i]['score']
        python_score = python_scores[i]
        diff = abs(tapir_score - python_score)
        status = "✓ MATCH" if diff < 0.001 else "✗ DIFF"
        
        print(f"{doc_id:6} | {python_score:12.6f} | {tapir_score:12.6f} | {diff:12.6f} | {status}")
    
    conn.close()

if __name__ == "__main__":
    detailed_bm25_debug()