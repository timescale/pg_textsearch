import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from benchmarks.scale.recover_audits import recover, validate_recovery


class AuditRecoveryTest(unittest.TestCase):
    def cells(self):
        return [
            {
                "cell_id": (
                    "high.english-stopwords.spans.search."
                    + kind + ".shape-filter_first"
                ),
                "analyzer": "english-stopwords",
                "pack": "spans",
                "operation": "search",
                "query_family": kind,
            }
            for kind in ("conjunction", "disjunction", "phrase")
        ]

    def state(self):
        return {
            "status": "infrastructure_error",
            "run_error": {
                "type": "JSONDecodeError",
                "message": "Expecting value: line 1 column 1 (char 0)",
            },
            "expected_cells": 28,
            "terminal_cells": 25,
            "cells": [
                {"cell_id": f"done-{i}", "status": "completed"}
                for i in range(25)
            ] + [
                {"cell_id": cell["cell_id"], "status": "pending"}
                for cell in self.cells()
            ],
        }

    def test_only_stopped_stopword_audit_parse_failures_are_recoverable(self):
        state = self.state()
        validate_recovery(state, self.cells())
        with self.assertRaisesRegex(ValueError, "parse failure"):
            validate_recovery(
                state | {"run_error": {"type": "OSError"}}, self.cells()
            )
        with self.assertRaisesRegex(ValueError, "stopword"):
            validate_recovery(
                state, [self.cells()[0] | {"pack": "natural"}]
            )

    def test_exact_three_recorded_pending_controls_are_required(self):
        with self.assertRaisesRegex(ValueError, "three"):
            validate_recovery(self.state(), self.cells()[:2])
        state = self.state()
        state["cells"][-1]["status"] = "completed"
        with self.assertRaisesRegex(ValueError, "recorded pending"):
            validate_recovery(state, self.cells())

    def test_no_pending_cells_is_not_a_repair(self):
        with self.assertRaisesRegex(ValueError, "pending"):
            validate_recovery(self.state(), [])

    def test_running_database_blocks_recovery(self):
        args = SimpleNamespace(root=Path(__file__).resolve().parents[3])
        with patch(
            "benchmarks.scale.recover_audits.subprocess.check_output",
            return_value="active-database\n",
        ):
            with self.assertRaisesRegex(ValueError, "idle Docker host"):
                recover(args)
