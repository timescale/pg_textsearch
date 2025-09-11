#!/usr/bin/env python3
"""
Debug BM25 calculations step by step to compare Tapir vs Python reference
"""

import math
import psycopg2
from psycopg2.extras import RealDictCursor
from rank_bm25 import BM25Okapi
import numpy as np

def debug_bm25_calculations():
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
        
    print("=== CORPUS DEBUG ===")
    for i, (doc_id, terms) in enumerate(zip(doc_ids, corpus)):
        print(f"Doc {doc_id}: {len(terms)} terms = {terms}")
    
    # Initialize BM25
    k1, b = 1.2, 0.75
    bm25 = BM25Okapi(corpus, k1=k1, b=b)
    
    print(f"\n=== BM25 PARAMETERS ===")
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
    
    print(f"\n=== QUERY DEBUG ===")
    print(f"Original query: '{test_query}'")
    print(f"Stemmed query terms: {query_terms}")
    
    # Debug IDF calculations
    print(f"\n=== IDF DEBUG ===")
    for term in query_terms:
        if term in bm25.idf:
            # Calculate manually to verify
            term_doc_freq = sum(1 for doc_freqs in bm25.doc_freqs if term in doc_freqs)
            manual_idf = math.log(bm25.corpus_size - term_doc_freq + 0.5) - math.log(term_doc_freq + 0.5)
            print(f"Term '{term}':")
            print(f"  Document frequency: {term_doc_freq} out of {bm25.corpus_size} docs")
            print(f"  Python IDF: {bm25.idf[term]:.6f}")
            print(f"  Manual IDF: {manual_idf:.6f}")
    
    # Debug BM25 scoring step by step
    print(f"\n=== STEP-BY-STEP BM25 SCORING ===")
    python_scores = bm25.get_scores(query_terms)
    
    print("Python BM25 scores:")
    for doc_id, score in zip(doc_ids, python_scores):
        print(f"  Doc {doc_id}: {score:.6f}")
    
    # Manual calculation to verify
    print(f"\n=== MANUAL CALCULATION VERIFICATION ===")
    for doc_idx, doc_id in enumerate(doc_ids):
        print(f"\nDoc {doc_id}:")
        doc_len = bm25.doc_len[doc_idx]
        print(f"  Document length: {doc_len}")
        print(f"  Length ratio (doc_len/avgdl): {doc_len/bm25.avgdl:.6f}")
        
        total_score = 0.0
        for term in query_terms:
            tf = bm25.doc_freqs[doc_idx].get(term, 0)  # term frequency in this doc
            idf = bm25.idf.get(term, 0)
            
            numerator = tf * (k1 + 1)
            denominator = tf + k1 * (1 - b + b * doc_len / bm25.avgdl)
            term_score = idf * (numerator / denominator)
            total_score += term_score
            
            print(f"    Term '{term}': tf={tf}, idf={idf:.6f}")
            print(f"      numerator = {tf} * ({k1} + 1) = {numerator:.6f}")
            print(f"      denominator = {tf} + {k1} * (1 - {b} + {b} * {doc_len} / {bm25.avgdl:.2f}) = {denominator:.6f}")
            print(f"      term_score = {idf:.6f} * ({numerator:.6f} / {denominator:.6f}) = {term_score:.6f}")
        
        print(f"    Total score: {total_score:.6f}")
        print(f"    Python score: {python_scores[doc_idx]:.6f}")
        print(f"    Difference: {abs(total_score - python_scores[doc_idx]):.6f}")
    
    # Now get Tapir scores for comparison
    print(f"\n=== TAPIR SCORES ===")
    with conn.cursor(cursor_factory=RealDictCursor) as cur:
        query_sql = """
            SELECT doc_id, (full_text <@> to_tpvector(%s, %s)) as score
            FROM aerodocs_documents
            ORDER BY doc_id
        """
        cur.execute(query_sql, (test_query, 'cranfield_tapir_idx'))
        tapir_results = cur.fetchall()
    
    print("Tapir scores (note: negative for PostgreSQL ordering):")
    for row in tapir_results:
        tapir_score = -row['score']  # Convert from negative
        doc_id = row['doc_id']
        python_score = python_scores[doc_ids.index(doc_id)]
        print(f"  Doc {doc_id}: Tapir={tapir_score:.6f}, Python={python_score:.6f}, Diff={abs(tapir_score-python_score):.6f}")

    conn.close()

if __name__ == "__main__":
    debug_bm25_calculations()