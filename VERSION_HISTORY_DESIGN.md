# README Version History

## Goal

Add a concise, durable version history to the README without restoring the
release-maintained current-version banner.

## Structure

- Add `## Version History` near the end of `README.md`, before Contributing.
- Use a two-column table: `Version` and `Highlights`.
- List every published release from `v1.4.0` through `v0.0.1`, newest first.
- Link every version to its GitHub release page.
- Summarize feature releases with one short phrase naming their major
  capabilities.
- Summarize patch releases at the stability or compatibility level rather than
  listing individual bug fixes.
- Omit dates, contributor lists, compare links, and an unreleased or current
  version row.

## Source of Truth

Use Git tags and GitHub release notes to establish the release list and
summaries. Do not infer features from version numbers.

## Validation

- The table contains every published tag exactly once.
- Every version link resolves to the matching GitHub release.
- Summaries describe shipped behavior and emphasize major features.
- The section remains concise and does not duplicate release-note detail.
