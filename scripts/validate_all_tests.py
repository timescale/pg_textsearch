#!/usr/bin/env python3
"""
Comprehensive BM25 validation for all Tapir test datasets.
Runs validation on every test table and reports results.
"""

import subprocess
import sys
import json
from typing import List, Dict, Tuple

# Test configurations for each dataset
TEST_CONFIGS = [
    {
        'name': 'Test Docs (English)',
        'table': 'test_docs',
        'text_column': 'content',
        'index_name': 'docs_english_idx',
        'text_config': 'english',
        'k1': 1.2,
        'b': 0.75,
        'queries': [
            'quick brown fox',
            'postgresql full text search',
            'lazy dog',
            'hello world',
            'sphinx quartz'
        ]
    },
    {
        'name': 'Test Docs (Simple)',
        'table': 'test_docs',
        'text_column': 'content',
        'index_name': 'docs_simple_idx',
        'text_config': 'simple',
        'k1': 1.5,
        'b': 0.8,
        'queries': [
            'the quick brown fox',
            'postgresql database',
            'the lazy dog'
        ]
    },
    {
        'name': 'Aerodocs Documents',
        'table': 'aerodocs_documents',
        'text_column': 'full_text',
        'index_name': 'cranfield_tapir_idx',
        'id_column': 'doc_id',
        'text_config': 'english',
        'k1': 1.2,
        'b': 0.75,
        'queries': [
            'aerodynamic flow analysis',
            'computational fluid dynamics',
            'wing design optimization',
            'turbulent boundary layer',
            'supersonic aircraft design'
        ]
    },
    {
        'name': 'Limit Test Dataset',
        'table': 'limit_test',
        'text_column': 'content',
        'index_name': 'limit_test_idx',
        'text_config': 'english',
        'k1': 1.2,
        'b': 0.75,
        'queries': [
            'database',
            'search',
            'optimization',
            'algorithm',
            'system',
            'database system',
            'search algorithm',
            'query optimization'
        ]
    },
    {
        'name': 'Long String Documents',
        'table': 'long_string_docs',
        'text_column': 'content',
        'index_name': 'long_strings_idx',
        'text_config': 'english',
        'k1': 1.2,
        'b': 0.75,
        'queries': [
            'https website',
            'postgresql extension',
            'file log error',
            'algorithm bm25',
            'postgresql tapir extension search'
        ]
    }
]

def run_validation(config: Dict, dbname: str = 'contrib_regression') -> Dict:
    """Run validation for a single test configuration."""
    
    print(f"\n{'='*80}")
    print(f"Testing: {config['name']}")
    print(f"{'='*80}")
    
    # Build command
    cmd = [
        'python3', 'scripts/bm25_validation.py',
        '--dbname', dbname,
        '--table', config['table'],
        '--text-column', config['text_column'],
        '--index-name', config['index_name'],
        '--text-config', config['text_config'],
        '--k1', str(config['k1']),
        '--b', str(config['b']),
        '--tolerance', '0.001',
        '--queries'
    ] + config['queries']
    
    # Add optional parameters
    if 'id_column' in config:
        cmd.extend(['--id-column', config['id_column']])
    
    # Run validation
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        
        # Parse output for summary statistics
        output_lines = result.stdout.split('\n')
        
        results = {
            'name': config['name'],
            'status': 'SUCCESS',
            'queries_tested': len(config['queries']),
            'details': []
        }
        
        # Extract statistics for each query
        current_query = None
        for i, line in enumerate(output_lines):
            if line.startswith("Query: '"):
                current_query = line[8:-1]
            elif "Matching scores" in line and current_query:
                # Extract match statistics
                parts = line.split(':')
                if len(parts) >= 2:
                    match_info = parts[1].strip()
                    results['details'].append({
                        'query': current_query,
                        'match_info': match_info
                    })
            elif "Score correlation:" in line and current_query:
                correlation = float(line.split(':')[1].strip())
                # Find the corresponding detail entry
                for detail in results['details']:
                    if detail['query'] == current_query and 'correlation' not in detail:
                        detail['correlation'] = correlation
                        break
        
        return results
        
    except subprocess.CalledProcessError as e:
        return {
            'name': config['name'],
            'status': 'FAILED',
            'error': e.stderr if e.stderr else str(e)
        }
    except Exception as e:
        return {
            'name': config['name'],
            'status': 'ERROR',
            'error': str(e)
        }

def main():
    """Run all validations and generate summary report."""
    
    print("="*80)
    print("TAPIR BM25 VALIDATION - COMPREHENSIVE TEST SUITE")
    print("="*80)
    
    # Check database connection
    dbname = 'contrib_regression'
    
    all_results = []
    total_tests = 0
    total_success = 0
    
    # Run each test configuration
    for config in TEST_CONFIGS:
        result = run_validation(config, dbname)
        all_results.append(result)
        
        if result['status'] == 'SUCCESS':
            total_success += 1
            print(f"✓ {config['name']}: ALL TESTS PASSED")
            
            # Show query details
            for detail in result.get('details', []):
                if 'correlation' in detail:
                    print(f"  - Query '{detail['query']}': {detail['match_info']} (correlation: {detail['correlation']:.6f})")
        else:
            print(f"✗ {config['name']}: FAILED")
            if 'error' in result:
                print(f"  Error: {result['error']}")
        
        total_tests += 1
    
    # Print summary report
    print("\n" + "="*80)
    print("SUMMARY REPORT")
    print("="*80)
    print(f"Total test configurations: {total_tests}")
    print(f"Successful: {total_success}")
    print(f"Failed: {total_tests - total_success}")
    
    if total_success == total_tests:
        print("\n✅ ALL VALIDATION TESTS PASSED!")
        print("Tapir's BM25 implementation matches the reference implementation perfectly.")
    else:
        print(f"\n⚠️  {total_tests - total_success} test(s) failed. Please review the errors above.")
    
    # Save detailed results to JSON
    with open('validation_results.json', 'w') as f:
        json.dump(all_results, f, indent=2)
        print(f"\nDetailed results saved to validation_results.json")
    
    return 0 if total_success == total_tests else 1

if __name__ == '__main__':
    sys.exit(main())