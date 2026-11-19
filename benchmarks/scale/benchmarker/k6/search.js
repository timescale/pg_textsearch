import db from "k6/x/database";
import exec from "k6/execution";
import { Counter, Rate } from "k6/metrics";

const engine = __ENV.ENGINE;
const mode = __ENV.MODE || "disjunction";
const limit = Number(__ENV.LIMIT || "10");
const duration = __ENV.DURATION || "30s";
const phase = __ENV.PHASE || "measured";
const vus = Number(__ENV.VUS || "2");
const queries = JSON.parse(open(__ENV.QUERY_FILE || "./natural.json"));
const rows = Number(__ENV.ROWS || "10000");
const updateTargets = mode === "mixed"
  ? JSON.parse(open("./update_targets.json"))
  : [];
const backends = db.backends({
  datasetPath: "../",
  backendsDir: __ENV.BACKENDS_DIR,
  backends: [engine],
});
backends.setTimeout(60);

const benchmarkErrors = new Rate("benchmark_errors");
const benchmarkTimeouts = new Rate("benchmark_timeouts");
const updateFailures = new Rate("update_failures");
const achievedUpdates = new Counter("achieved_updates");
const scenarios = {
  search: {
    executor: "constant-vus",
    vus,
    duration,
    exec: phase === "measured" ? "search" : "warmupSearch",
    tags: { backend: engine, chart: `${mode}_top${limit}` },
  },
};
if (phase === "measured" && mode === "mixed") {
  scenarios.updates = {
    executor: "constant-arrival-rate",
    rate: Number(__ENV.UPDATE_RATE || "10"),
    timeUnit: "1s",
    duration,
    preAllocatedVUs: 32,
    maxVUs: 64,
    exec: "updateDoc",
    tags: { backend: engine, chart: "mixed_updates" },
  };
}
export const options = {
  scenarios,
  thresholds: phase === "measured" ? {
    benchmark_errors: ["rate==0"],
    benchmark_timeouts: ["rate==0"],
    update_failures: ["rate==0"],
  } : {},
};
export const collectMetrics = phase === "measured"
  ? backends.addMetricsCollector(options.scenarios, duration)
  : () => ({});

const pgts = {
  disjunction: `SELECT id FROM documents ORDER BY
    body <@> to_bm25query($1, 'documents_bm25') LIMIT ${limit}`,
  conjunction: `SELECT id FROM documents
    WHERE body @@ plainto_tsquery('english', $1)
    ORDER BY body <@> to_bm25query($1, 'documents_bm25') LIMIT ${limit}`,
  phrase: `SELECT id FROM documents
    WHERE body @@ phraseto_tsquery('english', $1)
    ORDER BY body <@> to_bm25query($1, 'documents_bm25') LIMIT ${limit}`,
  count: `SELECT count(*) FROM documents WHERE body @@
    (SELECT string_agg(quote_literal(lexeme), ' | ')::tsquery
     FROM unnest(tsvector_to_array(to_tsvector('english', $1))) AS lexeme)
    AND (SELECT count(*) FROM unnest(
      tsvector_to_array(to_tsvector('english', $1))) AS lexeme) > 0`,
  mixed: `SELECT id FROM documents ORDER BY
    body <@> to_bm25query($1, 'documents_bm25') LIMIT ${limit}`,
};
const parade = {
  disjunction: `SELECT id FROM documents WHERE body ||| $1
    ORDER BY paradedb.score(id) DESC LIMIT ${limit}`,
  conjunction: `SELECT id FROM documents WHERE body &&& $1
    ORDER BY paradedb.score(id) DESC LIMIT ${limit}`,
  phrase: `SELECT id FROM documents WHERE body ### $1
    ORDER BY paradedb.score(id) DESC LIMIT ${limit}`,
  count: `SELECT count(*) FROM documents WHERE body ||| $1`,
  mixed: `SELECT id FROM documents WHERE body ||| $1
    ORDER BY paradedb.score(id) DESC LIMIT ${limit}`,
};

function runSearch(recordMetrics) {
  const query = queries[exec.scenario.iterationInTest % queries.length];
  exec.vu.metrics.tags.query_id = String(query.id);
  let text = query.text;
  let statement;
  if (engine === "pg_textsearch") statement = pgts[mode];
  else if (engine === "paradedb") statement = parade[mode];
  else statement = `SELECT id FROM documents WHERE body ==> $1
    ORDER BY tin.full_score(ctid) DESC LIMIT ${limit}`;
  const result = backends.get(engine).query(statement, text);
  if (recordMetrics) {
    benchmarkErrors.add(Boolean(result.error));
    benchmarkTimeouts.add(Boolean(result.deadlineReached));
  }
}
export function warmupSearch() { runSearch(false); }
export function search() { runSearch(true); }

export function updateDoc() {
  const id = updateTargets[
    exec.scenario.iterationInTest % updateTargets.length
  ];
  const result = backends.get(engine).query(
    `UPDATE documents SET body = CASE
       WHEN right(body, 11) = ' pgtsupdate'
       THEN left(body, length(body) - 11)
       ELSE body || ' pgtsupdate'
     END WHERE id = $1 RETURNING id`,
    id,
  );
  const succeeded = !result.error && result.hits === 1;
  updateFailures.add(!succeeded);
  if (succeeded) achievedUpdates.add(1);
  benchmarkErrors.add(Boolean(result.error));
  benchmarkTimeouts.add(Boolean(result.deadlineReached));
}
