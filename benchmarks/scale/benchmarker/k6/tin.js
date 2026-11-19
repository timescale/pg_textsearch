import db from "k6/x/database";
import exec from "k6/execution";
import { Counter, Rate } from "k6/metrics";

const engine = __ENV.ENGINE;
const operation = __ENV.OPERATION;
const queryFamily = __ENV.QUERY_FAMILY;
const cellId = __ENV.CELL_ID;
const pack = __ENV.PACK;
const profile = __ENV.PROFILE;
const analyzer = __ENV.ANALYZER;
const duration = __ENV.DURATION || "30s";
const phase = __ENV.PHASE || "measured";
const vus = Number(__ENV.VUS || "8");
const scenario = JSON.parse(open(__ENV.SCENARIO_FILE));
const queries = JSON.parse(open(__ENV.QUERY_FILE));
const updateTargets = operation === "concurrent-writes"
  ? JSON.parse(open(__ENV.UPDATE_TARGETS_FILE))
  : [];
const kinds = scenario.kinds;
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
const commonTags = {
  backend: engine,
  cell_id: cellId,
  pack, profile, analyzer,
};
const scenarios = {
  queries: {
    executor: "constant-vus",
    vus,
    duration,
    gracefulStop: "65s",
    exec: phase === "measured" ? "search" : "warmupSearch",
    tags: {
      ...commonTags,
      operation,
      query_family: queryFamily,
    },
  },
};
if (phase === "measured" && operation === "concurrent-writes") {
  scenarios.updates = {
    executor: "constant-arrival-rate",
    rate: Number(__ENV.UPDATE_RATE || "1000"),
    timeUnit: "1s",
    duration,
    gracefulStop: "65s",
    preAllocatedVUs: 32,
    maxVUs: 64,
    exec: "updateDoc",
    tags: {
      ...commonTags,
      operation: "update",
      query_family: queryFamily,
    },
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

function argumentsFor(query, kind) {
  if (engine === "paradedb") {
    return [kind === "phrase" ? JSON.stringify(query.text) : query.text];
  }
  if (operation === "count") return [query.pg_tsquery[kind]];
  if (kind === "disjunction") return [query.text];
  return [query.text, query.pg_tsquery[kind]];
}

function selectQuery(iteration) {
  const kindCount = kinds.length;
  const queryCount = queries.length;
  const kindIndex = iteration % kindCount;
  const queryIndex = (
    Math.floor(iteration / kindCount)
    + kindIndex * Math.floor(queryCount / kindCount)
  ) % queryCount;
  return { query: queries[queryIndex], kind: kinds[kindIndex] };
}

function runSearch(recordMetrics) {
  const iteration = exec.scenario.iterationInTest;
  const { query, kind } = selectQuery(iteration);
  exec.vu.metrics.tags.query_id = `${query.id}:${kind}`;
  exec.vu.metrics.tags.query_kind = kind;
  const result = backends.get(engine).query(
    scenario.sql[kind],
    ...argumentsFor(query, kind),
  );
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
