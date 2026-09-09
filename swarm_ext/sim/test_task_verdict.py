"""Task verdicts must retain their identity and fail before flight cleanup."""

import copy
import unittest

from run_scenario import run_scenario, task_matches


class TaskVerdictTest(unittest.TestCase):
    def setUp(self):
        self.expected = {
            "running": False,
            "outcome": "Succeeded",
            "completed_ids": [2],
            "withdrawn_ids": [1],
            "disarmed_ids": [2],
            "required_dispatches": [{"stage_index": 1, "vehicle_id": 2, "type": "rtl"}],
            "no_dispatch_after_withdrawal": True,
        }
        self.status = {
            "ok": True,
            "task": {
                "task_id": "task-a",
                "running": False,
                "outcome": "Succeeded",
                "result": {
                    "task_id": "task-a",
                    "outcome": "Succeeded",
                    "completed_ids": [2],
                    "withdrawn_ids": [1],
                    "dispatches": [{"stage_index": 1, "vehicle_id": 2, "type": "rtl"}],
                    "withdrawal_offsets": {"1": 0},
                },
            },
            "vehicles": [{"id": 2, "armed": False}],
        }

    def test_success_requires_all_evidence(self):
        self.assertTrue(task_matches(self.status, self.expected, "task-a"))
        for outcome in ("Failed", "Aborted", "None"):
            status = copy.deepcopy(self.status)
            status["task"]["outcome"] = outcome
            status["task"]["result"]["outcome"] = outcome
            self.assertFalse(task_matches(status, self.expected, "task-a"))
        self.assertFalse(task_matches(self.status, self.expected, "other-task"))
        self.assertFalse(task_matches(self.status, self.expected, ""))

    def test_missing_dispatch_withdrawal_or_disarm_fails(self):
        for key in ("completed_ids", "withdrawn_ids", "dispatches", "withdrawal_offsets"):
            status = copy.deepcopy(self.status)
            del status["task"]["result"][key]
            self.assertFalse(task_matches(status, self.expected, "task-a"), key)
        status = copy.deepcopy(self.status)
        status["vehicles"][0]["armed"] = True
        self.assertFalse(task_matches(status, self.expected, "task-a"))

    def test_late_dispatch_to_retired_member_fails(self):
        self.status["task"]["result"]["dispatches"].append(
            {"stage_index": 1, "vehicle_id": 1, "type": "rtl"}
        )
        self.assertFalse(task_matches(self.status, self.expected, "task-a"))

    def test_real_runner_cleanup_cannot_mask_failure(self):
        status = copy.deepcopy(self.status)
        status["task"]["outcome"] = "Failed"
        status["task"]["result"]["outcome"] = "Failed"

        class Client:
            def request(self, command, **kwargs):
                return {"ok": True, "status": status}

            def wait_status(self, predicate, timeout):
                return status

        scenario = {
            "name": "terminal failure followed by successful cleanup",
            "fleet": 2,
            "requires_qgc": True,
            "steps": [
                {"action": "qgc_start_task", "stages": []},
                {"action": "qgc_expect_task", **self.expected},
                {"action": "qgc_abort_task"},
            ],
        }
        result = run_scenario(scenario, "unused", False, Client())
        self.assertFalse(result["pass"])
        self.assertTrue(result["steps"][-1]["ok"])


if __name__ == "__main__":
    unittest.main()
