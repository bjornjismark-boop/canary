from copy import deepcopy
import unittest

from tools.playerbots_live_soak import (
    classify_release_summary,
    render_summary_markdown,
)


def qualifying_release() -> dict[str, object]:
    return {
        "result": "PASS",
        "profile": "release",
        "durationSeconds": 7200,
        "configuredBots": 20,
        "peakManagedPopulation": 20,
        "humanParticipants": 1,
        "peakOrdinaryPlayers": 1,
        "restartCount": 3,
        "ordinaryClientPlacements": 4,
        "ordinaryActivityCount": 24,
        "ordinaryConnectedPast1000Seconds": True,
        "unexpectedOrdinaryClientExits": 0,
        "managedPingTimeouts": 0,
        "unexpectedManagedLogouts": 0,
        "unexpectedManagedLogins": 0,
        "unscheduledSessionReplacements": 0,
        "duplicateSessions": 0,
        "invariantFailureCount": 0,
        "rssInitialKb": 1,
        "rssPeakKb": 2,
        "rssFinalKb": 1,
        "tickSampleCount": 100,
        "cleanup": "PASS",
    }


class ReleaseClassificationTest(unittest.TestCase):
    def test_qualifying_release_is_production_pass(self) -> None:
        summary = qualifying_release()

        production, release = classify_release_summary(summary)

        self.assertTrue(production)
        self.assertEqual(release, "PASS")

        summary["productionSoak"] = production
        summary["releaseSoak"] = release
        markdown = render_summary_markdown(summary)

        self.assertIn("productionSoak: True", markdown)
        self.assertIn("releaseSoak: PASS", markdown)
        self.assertTrue(markdown.endswith("PRODUCTION_SOAK=YES\n"))

    def test_smoke_is_not_release_evidence(self) -> None:
        summary = qualifying_release()
        summary["profile"] = "smoke"

        production, release = classify_release_summary(summary)

        self.assertFalse(production)
        self.assertEqual(release, "NOT_RUN")

        summary["productionSoak"] = production
        summary["releaseSoak"] = release
        markdown = render_summary_markdown(summary)

        self.assertIn("releaseSoak: NOT_RUN", markdown)
        self.assertTrue(markdown.endswith("PRODUCTION_SOAK=NO\n"))

    def test_interrupted_release_is_fail(self) -> None:
        summary = qualifying_release()
        summary["result"] = "FAIL"
        summary["restartCount"] = 0

        production, release = classify_release_summary(summary)

        self.assertFalse(production)
        self.assertEqual(release, "FAIL")

        summary["productionSoak"] = production
        summary["releaseSoak"] = release
        self.assertTrue(
            render_summary_markdown(summary).endswith(
                "PRODUCTION_SOAK=NO\n"
            )
        )

    def test_each_required_release_condition_is_enforced(self) -> None:
        bad_cases = {
            "runtime failure": ("result", "FAIL"),
            "wrong duration": ("durationSeconds", 7199),
            "wrong configured population": ("configuredBots", 19),
            "missing peak population": ("peakManagedPopulation", 19),
            "no ordinary participant": ("humanParticipants", 0),
            "no ordinary placement": ("peakOrdinaryPlayers", 0),
            "too few restarts": ("restartCount", 2),
            "missing reconnect placement": (
                "ordinaryClientPlacements",
                3,
            ),
            "no ordinary activity": ("ordinaryActivityCount", 0),
            "idle boundary not crossed": (
                "ordinaryConnectedPast1000Seconds",
                False,
            ),
            "ordinary client exit": (
                "unexpectedOrdinaryClientExits",
                1,
            ),
            "managed ping timeout": ("managedPingTimeouts", 1),
            "managed logout": ("unexpectedManagedLogouts", 1),
            "managed login": ("unexpectedManagedLogins", 1),
            "session replacement": (
                "unscheduledSessionReplacements",
                1,
            ),
            "duplicate session": ("duplicateSessions", 1),
            "invariant failure": ("invariantFailureCount", 1),
            "missing initial rss": ("rssInitialKb", None),
            "missing peak rss": ("rssPeakKb", None),
            "missing final rss": ("rssFinalKb", None),
            "missing dispatcher samples": ("tickSampleCount", 0),
            "cleanup failure": ("cleanup", "FAIL"),
        }

        for name, (field, value) in bad_cases.items():
            with self.subTest(name=name):
                summary = deepcopy(qualifying_release())
                summary[field] = value

                production, release = classify_release_summary(summary)

                self.assertFalse(production)
                self.assertEqual(release, "FAIL")


if __name__ == "__main__":
    unittest.main()
