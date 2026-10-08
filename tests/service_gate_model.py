#!/usr/bin/env python3
"""Reproducible model of service.c's atomic commit/cancel/stop linearization.

Run: python3 tests/service_gate_model.py
This does NOT run production C, Windows atomics, SCM, IPC, or power APIs. Native
race tests in docs/SERVICE.md remain mandatory. Each named CAS is one atomic step;
ordinary work may occur before/after it in any order preserving each thread.
"""
import unittest

CANCEL = 1
STOP = 2
COMMITTED = 0x40000000


def interleavings(left, right):
    if not left:
        yield right
    elif not right:
        yield left
    else:
        for tail in interleavings(left[1:], right):
            yield (left[0],) + tail
        for tail in interleavings(left, right[1:]):
            yield (right[0],) + tail


class ServiceGateModel(unittest.TestCase):
    def test_cancel_submission_interleavings(self):
        count = 0
        for order in interleavings(
            ("validate", "commit_cas", "power_call"),
            ("cancel_cas", "pause"),
        ):
            with self.subTest(order=order):
                gate, calls = 0, 0
                phase, commit_won, cancel_accepted = "ready", False, None
                for event in order:
                    if event == "commit_cas":
                        commit_won = gate == 0
                        if commit_won:
                            gate = COMMITTED
                    elif event == "cancel_cas":
                        cancel_accepted = not bool(gate & COMMITTED)
                        if cancel_accepted:
                            gate |= CANCEL
                    elif event == "pause" and cancel_accepted:
                        phase = "paused"
                    elif event == "power_call" and commit_won:
                        calls += 1
                        phase = "requested"
                self.assertFalse(cancel_accepted and calls)
                self.assertFalse(calls and phase == "paused")
                if calls:
                    self.assertIs(cancel_accepted, False)  # Honest too-late response.
                count += 1
        self.assertEqual(count, 10)

    def test_stop_is_allowed_before_and_after_commit(self):
        count = 0
        for order in interleavings(
            ("validate", "commit_cas", "power_call"),
            ("stop_inhibit", "stop_event"),
        ):
            with self.subTest(order=order):
                gate, calls = 0, 0
                commit_won, stopped, stop_won = False, False, None
                for event in order:
                    if event == "commit_cas":
                        commit_won = gate == 0
                        if commit_won:
                            gate = COMMITTED
                    elif event == "stop_inhibit":
                        stop_won = not bool(gate & COMMITTED)
                        if stop_won:
                            gate |= STOP
                    elif event == "stop_event":
                        stopped = True  # Stop always succeeds, even after commit.
                    elif event == "power_call" and commit_won:
                        calls += 1
                self.assertTrue(stopped)
                if stop_won:
                    self.assertEqual(calls, 0)
                else:
                    # Stopping a monitor does not withdraw an already committed
                    # request, including one Windows/applications later abort.
                    self.assertEqual(calls, 1)
                count += 1
        self.assertEqual(count, 10)


if __name__ == "__main__":
    unittest.main(verbosity=2)
