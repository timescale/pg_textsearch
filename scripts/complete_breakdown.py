#!/usr/bin/env python3
"""
Complete step-by-step BM25 calculation breakdown for exact divergence analysis
"""

import math
import psycopg2
from psycopg2.extras import RealDictCursor
from rank_bm25 import BM25Okapi
import numpy as np

def complete_breakdown():
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
    raw_documents = {}
    
    print("=== STEP 1: CORPUS PREPROCESSING ===")
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
        raw_documents[row['doc_id']] = {
            'original_text': row['original_text'],
            'tsvector': tsvector_text,
            'terms': terms,
            'term_frequencies': dict((t, terms.count(t)) for t in set(terms))
        }
    
    # Initialize BM25
    k1, b = 1.2, 0.75
    bm25 = BM25Okapi(corpus, k1=k1, b=b)
    
    print("=== STEP 2: PYTHON BM25 INITIALIZATION ===")
    print(f"k1 = {k1}")
    print(f"b = {b}")
    print(f"corpus_size = {bm25.corpus_size}")
    print(f"document lengths = {bm25.doc_len}")
    print(f"avgdl = {bm25.avgdl}")
    print(f"total_corpus_length = {sum(bm25.doc_len)}")
    print(f"manual avgdl = {sum(bm25.doc_len) / len(bm25.doc_len)}")
    
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
    
    print(f"\n=== STEP 3: QUERY PROCESSING ===")
    print(f"Original query: '{test_query}'")
    print(f"Query tsvector: {tsvector_result}")
    print(f"Query terms: {query_terms}")
    
    # Analyze target document (Doc 1)
    target_doc_id = 1
    target_doc_idx = doc_ids.index(target_doc_id)
    target_doc = raw_documents[target_doc_id]
    
    print(f"\n=== STEP 4: TARGET DOCUMENT ANALYSIS (Doc {target_doc_id}) ===")
    print(f"Original text: '{target_doc['original_text']}'")
    print(f"Document tsvector: {target_doc['tsvector']}")
    print(f"Document terms: {target_doc['terms']}")
    print(f"Document term frequencies: {target_doc['term_frequencies']}")
    print(f"Document length: {len(target_doc['terms'])}")
    print(f"Python doc_len: {bm25.doc_len[target_doc_idx]}")
    print(f"Length match: {len(target_doc['terms']) == bm25.doc_len[target_doc_idx]}")
    
    # Calculate IDF for each query term
    print(f"\n=== STEP 5: IDF CALCULATION BREAKDOWN ===")
    
    # First, calculate document frequencies manually
    term_doc_freqs = {}
    for term in query_terms:
        doc_freq = sum(1 for doc_freqs in bm25.doc_freqs if term in doc_freqs)
        term_doc_freqs[term] = doc_freq
        print(f"\\nTerm '{term}':")
        print(f"  Documents containing term: {doc_freq} out of {bm25.corpus_size}")
        
        # Manual IDF calculation
        numerator = bm25.corpus_size - doc_freq + 0.5
        denominator = doc_freq + 0.5
        ratio = numerator / denominator
        raw_idf = math.log(ratio)
        
        print(f"  Manual IDF:")
        print(f"    numerator = corpus_size - doc_freq + 0.5 = {bm25.corpus_size} - {doc_freq} + 0.5 = {numerator}")
        print(f"    denominator = doc_freq + 0.5 = {doc_freq} + 0.5 = {denominator}")
        print(f"    ratio = {numerator} / {denominator} = {ratio}")
        print(f"    raw_idf = ln({ratio}) = {raw_idf}")
        
        # Python BM25 IDF (with epsilon handling)
        python_idf = bm25.idf.get(term, 0)
        print(f"  Python BM25 IDF: {python_idf}")
        
        if raw_idf < 0:
            print(f"  >>> EPSILON APPLIED: raw_idf < 0, Python uses epsilon = {python_idf}")
            print(f"  >>> Python epsilon = 0.25 * average_idf = 0.25 * {bm25.average_idf} = {0.25 * bm25.average_idf}")
        else:
            print(f"  >>> NO EPSILON: raw_idf >= 0, Python uses raw_idf")
    
    # Calculate BM25 score step by step for target document
    print(f"\\n=== STEP 6: BM25 SCORING BREAKDOWN (Doc {target_doc_id}) ===")
    doc_len = bm25.doc_len[target_doc_idx]
    doc_freqs = bm25.doc_freqs[target_doc_idx]
    
    print(f"Document length: {doc_len}")
    print(f"Average document length: {bm25.avgdl}")
    print(f"Length normalization factor: doc_len/avgdl = {doc_len}/{bm25.avgdl} = {doc_len/bm25.avgdl}")
    print(f"Document term frequencies: {doc_freqs}")
    
    total_score = 0.0
    print(f"\\nTerm-by-term scoring:")
    
    for i, term in enumerate(query_terms):
        tf = doc_freqs.get(term, 0)
        idf = bm25.idf.get(term, 0)
        
        print(f"\\n  Term {i+1}: '{term}'")
        print(f"    tf (term frequency in document) = {tf}")
        print(f"    idf (inverse document frequency) = {idf}")
        
        # BM25 formula components
        numerator = tf * (k1 + 1)
        denominator = tf + k1 * (1 - b + b * doc_len / bm25.avgdl)
        tf_component = numerator / denominator
        term_score = idf * tf_component
        
        print(f"    BM25 components:")
        print(f"      numerator = tf * (k1 + 1) = {tf} * ({k1} + 1) = {numerator}")
        print(f"      denominator = tf + k1 * (1 - b + b * doc_len / avgdl)")
        print(f"                  = {tf} + {k1} * (1 - {b} + {b} * {doc_len} / {bm25.avgdl})")
        print(f"                  = {tf} + {k1} * (1 - {b} + {b * doc_len / bm25.avgdl})")
        print(f"                  = {tf} + {k1} * {1 - b + b * doc_len / bm25.avgdl}")
        print(f"                  = {tf} + {k1 * (1 - b + b * doc_len / bm25.avgdl)}")
        print(f"                  = {denominator}")
        print(f"      tf_component = numerator / denominator = {numerator} / {denominator} = {tf_component}")
        print(f"      term_score = idf * tf_component = {idf} * {tf_component} = {term_score}")
        
        total_score += term_score
        print(f"    running_total = {total_score}")
    
    print(f"\\n  FINAL PYTHON SCORE: {total_score}")
    
    # Get Python library score for verification
    python_scores = bm25.get_scores(query_terms)
    python_lib_score = python_scores[target_doc_idx]
    print(f"  PYTHON LIBRARY SCORE: {python_lib_score}")
    print(f"  Manual vs Library match: {abs(total_score - python_lib_score) < 0.000001}")
    
    # Now get Tapir score with detailed debug
    print(f"\\n=== STEP 7: TAPIR SCORING COMPARISON ===")
    
    with conn.cursor() as cur:
        cur.execute("SET client_min_messages = DEBUG1")
        cur.execute("SELECT (full_text <@> to_tpvector(%s, %s)) as score FROM aerodocs_documents WHERE doc_id = %s",
                   (test_query, 'cranfield_tapir_idx', target_doc_id))
        tapir_score = cur.fetchone()[0]
    
    print(f"TAPIR SCORE: {tapir_score}")
    print(f"PYTHON SCORE: {python_lib_score}")
    print(f"DIFFERENCE: {abs(tapir_score - python_lib_score)}")
    print(f"RATIO: {tapir_score / python_lib_score if python_lib_score != 0 else 'N/A'}")
    
    conn.close()

if __name__ == "__main__":
    complete_breakdown()