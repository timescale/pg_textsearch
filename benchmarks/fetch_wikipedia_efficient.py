#!/usr/bin/env python3
"""
Efficiently fetch large numbers of Wikipedia articles
Uses the Wikipedia API query generator for better performance
"""

import json
import sys
import urllib.request
import urllib.parse
import time

def fetch_wikipedia_efficiently(target_count, output_file):
    """Fetch Wikipedia articles efficiently using the API"""

    api_url = "https://en.wikipedia.org/w/api.php"
    articles_fetched = 0

    with open(output_file, 'w', encoding='utf-8') as out:
        # Use different generators for variety
        generators = [
            ('random', {}),  # Random articles
            ('categorymembers', {'gcmtitle': 'Category:Featured_articles'}),
            ('categorymembers', {'gcmtitle': 'Category:Technology'}),
            ('categorymembers', {'gcmtitle': 'Category:Science'}),
            ('categorymembers', {'gcmtitle': 'Category:History'}),
            ('categorymembers', {'gcmtitle': 'Category:Geography'}),
        ]

        generator_index = 0

        while articles_fetched < target_count:
            generator_type, extra_params = generators[generator_index % len(generators)]
            generator_index += 1

            # Fetch up to 50 articles at once (API maximum for extracts)
            batch_size = min(50, target_count - articles_fetched)

            params = {
                'action': 'query',
                'format': 'json',
                'generator': generator_type,
                'prop': 'extracts|pageprops',
                'exintro': True,
                'explaintext': True,
                'exlimit': batch_size,
                'exchars': 2000,  # Limit extract size
                'grnnamespace': 0 if generator_type == 'random' else None,
                'grnlimit': batch_size if generator_type == 'random' else None,
                'gcmlimit': batch_size if generator_type == 'categorymembers' else None,
            }

            # Remove None values and add extra params
            params = {k: v for k, v in params.items() if v is not None}
            params.update(extra_params)

            # Build URL
            url = api_url + '?' + urllib.parse.urlencode(params)

            # Add User-Agent header
            req = urllib.request.Request(url)
            req.add_header('User-Agent', 'pg_textsearch-benchmark/1.0')

            try:
                with urllib.request.urlopen(req) as response:
                    data = json.loads(response.read().decode())

                    if 'query' in data and 'pages' in data['query']:
                        batch_articles = 0
                        for page_id, page in data['query']['pages'].items():
                            if articles_fetched >= target_count:
                                break

                            title = page.get('title', '')
                            extract = page.get('extract', '')

                            # Skip redirects and disambiguation pages
                            pageprops = page.get('pageprops', {})
                            if 'disambiguation' in pageprops:
                                continue

                            # Clean and validate
                            if title and extract and len(extract) > 100:
                                # Clean for TSV
                                title = title.replace('\t', ' ').replace('\n', ' ').replace('\r', '')
                                extract = extract.replace('\t', ' ').replace('\n', ' ').replace('\r', '')

                                # Limit length
                                if len(extract) > 5000:
                                    extract = extract[:5000]

                                out.write(f"{title}\t{extract}\n")
                                articles_fetched += 1
                                batch_articles += 1

                        if batch_articles > 0 and articles_fetched % 100 == 0:
                            print(f"Fetched {articles_fetched}/{target_count} articles...")

                # Rate limiting
                time.sleep(0.2)  # Be nice to Wikipedia's servers

            except Exception as e:
                print(f"Error fetching batch: {e}")
                time.sleep(2)  # Wait longer on error
                continue

    return articles_fetched

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: fetch_wikipedia_efficient.py <count> <output_file>")
        print("Example: fetch_wikipedia_efficient.py 100000 wikipedia_100k.tsv")
        sys.exit(1)

    count = int(sys.argv[1])
    output_file = sys.argv[2]

    print(f"Fetching {count} Wikipedia articles efficiently...")
    print("This will take approximately {} minutes at 0.2s per batch of 50...".format(count // 50 * 0.2 / 60))

    fetched = fetch_wikipedia_efficiently(count, output_file)

    print(f"\nSuccessfully fetched {fetched} articles")

    # Show file size
    import os
    if os.path.exists(output_file):
        size = os.path.getsize(output_file)
        print(f"File size: {size/1024/1024:.1f} MB")
        print(f"Average size: {size/fetched:.0f} bytes per article")
