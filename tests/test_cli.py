"""The publisher and a stand-in game, end to end, with a directory standing in for storage.

    python3 -I tests/test_cli.py BUILD_DIR

Optional: LIVEOPS_STACK_BIN (a baker-dev stacks_ledger binary) and LIVEOPS_LEDGER (its
stacks/ledger directory) add the Ledger scenario gate and retire through the stack.
LIVEOPS_BACKEND (cmd:...) runs the same flow through another backend, as the Nakama job does.
Keys are made fresh in a temporary directory and never printed.
"""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(sys.argv.pop(1) if len(sys.argv) > 1 else ROOT / "build").resolve()
TOOL = str(BUILD / "publish" / "baker-liveops")
HOST = str(BUILD / "tests" / "liveops_host")
GAME = str(ROOT / "tests" / "game" / "liveops.dta")
BASE = str(ROOT / "tests" / "game")
OVERLAYS = ROOT / "tests" / "overlays"


def run(*args, env=None):
    return subprocess.run([str(a) for a in args], capture_output=True, text=True, env=env)


class Flow(unittest.TestCase):
    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="liveops-cli-"))
        self.key = self.dir / "signing.key"
        r = run(TOOL, "keygen", "--out", self.key)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.pub = run(TOOL, "pubkey", "--key", self.key).stdout.strip()
        self.backend = os.environ.get("LIVEOPS_BACKEND") or f"static:{self.dir / 'cdn'}"
        self.env = "prod-" + self.dir.name[-8:] if os.environ.get("LIVEOPS_BACKEND") else "prod"

    def tearDown(self):
        shutil.rmtree(self.dir)

    def publish(self, overlay, policy, *extra):
        return run(TOOL, "publish", "--game", GAME, "--base", BASE, "--overlay", OVERLAYS / overlay,
                   "--policy", policy, "--backend", self.backend, "--env", self.env, "--key", self.key, *extra)

    def host(self, *extra, store="user"):
        return run(HOST, "--game", GAME, "--base", BASE, "--backend", self.backend, "--env", self.env,
                   "--trust", self.pub, "--store", self.dir / store, *extra)

    def test_publish_apply_withdraw(self):
        r = self.publish("balance_2026_10.dta", "hotfix")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("economy.gold_per_win: 100 -> 120", r.stdout)
        r = self.publish("double_xp.dta", "events")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

        h = self.host("--get", "economy.gold_per_win", "--get", "events.banner")
        self.assertEqual(h.returncode, 0, h.stdout + h.stderr)
        self.assertIn("live balance_2026_10 3 origin=ops/balance_2026_10.dta", h.stdout)
        self.assertIn("live double_xp 1 origin=ops/double_xp.dta", h.stdout)
        self.assertIn("value economy.gold_per_win = (gold_per_win 120) (layer balance_2026_10", h.stdout)
        self.assertIn("origin ops/balance_2026_10.dta", h.stdout)

        r = run(TOOL, "show", "--backend", self.backend, "--env", self.env, "--trust", self.pub)
        self.assertIn("sequence 2", r.stdout)

        r = run(TOOL, "withdraw", "double_xp", "--backend", self.backend, "--env", self.env, "--key", self.key)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        h = self.host("--get", "events.banner")
        self.assertIn("log: removed double_xp 1", h.stdout)
        self.assertNotIn("live double_xp", h.stdout)
        self.assertIn("value events.banner = (none)", h.stdout)

        # Offline, the next start takes the last good set from the store.
        h = self.host("--offline", "--get", "economy.gold_per_win", "--mode", "tree")
        self.assertIn("live balance_2026_10 3", h.stdout)
        self.assertIn("(gold_per_win 120)", h.stdout)

    def test_publisher_refuses_what_the_game_would(self):
        for overlay, policy, reason in [
            ("adds_command.dta", "hotfix", "may hold data only"),
            ("outside_allow.dta", "hotfix", "outside the paths"),
            ("newer_base.dta", "hotfix", "requires 'game_data' \"1.5\""),
            ("bad_shape.dta", "events", "outside 0.0 to 1.0"),
            ("balance_2026_10.dta", "nonesuch", "no policy named nonesuch"),
        ]:
            r = self.publish(overlay, policy)
            self.assertEqual(r.returncode, 1, overlay + ": " + r.stdout + r.stderr)
            self.assertIn(reason, r.stdout, overlay)
        r = run(TOOL, "show", "--backend", self.backend, "--env", self.env, "--trust", self.pub)
        self.assertIn("has no manifest", r.stdout)

    def test_a_manifest_from_another_key_stops_the_publisher(self):
        self.assertEqual(self.publish("balance_2026_10.dta", "hotfix").returncode, 0)
        other = self.dir / "other.key"
        run(TOOL, "keygen", "--out", other)
        r = run(TOOL, "publish", "--game", GAME, "--base", BASE, "--overlay", OVERLAYS / "double_xp.dta",
                "--policy", "events", "--backend", self.backend, "--env", self.env, "--key", other)
        self.assertEqual(r.returncode, 2)
        self.assertIn("does not verify", r.stderr)
        # Trusting the first key, the second may publish on top.
        r = run(TOOL, "publish", "--game", GAME, "--base", BASE, "--overlay", OVERLAYS / "double_xp.dta",
                "--policy", "events", "--backend", self.backend, "--env", self.env, "--key", other,
                "--trust", self.pub)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_keys_stay_out_of_output(self):
        seed = self.key.read_text().strip()
        r = self.publish("balance_2026_10.dta", "hotfix")
        k = run(TOOL, "keygen", "--out", self.key)
        self.assertNotEqual(k.returncode, 0)  # never overwritten
        for out in (r.stdout, r.stderr, k.stdout, k.stderr):
            self.assertNotIn(seed, out)

    def test_retire_writes_a_patch_and_nothing_else(self):
        work = self.dir / "game"
        shutil.copytree(BASE, work)
        patch = self.dir / "retire.patch"
        r = run(TOOL, "retire", "--game", GAME, "--base", work, "--overlay", OVERLAYS / "balance_2026_10.dta",
                "--patch", patch)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        text = patch.read_text()
        changed = [l for l in text.splitlines() if l[:1] in "+-" and not l.startswith(("---", "+++"))]
        self.assertEqual(changed, ["-   (gold_per_win 100)", "-   (shop_discount 0))",
                                   "+   (gold_per_win 120)", "+   (shop_discount 0.1))"])
        self.assertEqual((work / "data" / "default.dta").read_text(),
                         (pathlib.Path(BASE) / "data" / "default.dta").read_text())
        r = subprocess.run(["patch", "-p1", "-s", "-i", str(patch)], cwd=work, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("(gold_per_win 120)", (work / "data" / "default.dta").read_text())


@unittest.skipUnless(os.environ.get("LIVEOPS_STACK_BIN") and os.environ.get("LIVEOPS_LEDGER"),
                     "LIVEOPS_STACK_BIN and LIVEOPS_LEDGER name a Ledger build and checkout")
class Ledger(unittest.TestCase):
    def setUp(self):
        self.bin = os.environ["LIVEOPS_STACK_BIN"]
        self.ledger = pathlib.Path(os.environ["LIVEOPS_LEDGER"])
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="liveops-ledger-"))
        run(TOOL, "keygen", "--out", self.dir / "k")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def publish(self, overlay):
        return run(TOOL, "publish", "--game", ROOT / "tests/ledger/liveops.dta", "--base", self.ledger,
                   "--overlay", ROOT / "tests/ledger" / overlay, "--policy", "balance",
                   "--backend", f"static:{self.dir / 'cdn'}", "--env", "prod", "--key", self.dir / "k",
                   "--stack", self.bin, "--stack-data", self.ledger / "data")

    def test_an_overlay_that_breaks_an_objective_is_refused(self):
        r = self.publish("cheap_tailor.dta")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("an objective that passes on the base breaks with it", r.stdout)
        self.assertIn('"name": "ledger.pace_tailor", "moved": "broke"', r.stdout)
        self.assertFalse((self.dir / "cdn" / "prod" / "manifest").exists())

    def test_an_overlay_that_keeps_them_is_published(self):
        r = self.publish("slower_glut.dta")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("scenarios: no objective broke", r.stdout)

    def test_retire_through_the_stack_and_through_baker_agree(self):
        a, b = self.dir / "a.patch", self.dir / "b.patch"
        overlay = ROOT / "tests/ledger/slower_glut.dta"
        r = run(TOOL, "retire", "--game", ROOT / "tests/ledger/liveops.dta", "--base", self.ledger,
                "--overlay", overlay, "--patch", a)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = run(TOOL, "retire", "--stack", self.bin, "--stack-data", self.ledger / "data",
                "--overlay", overlay, "--patch", b)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        body = lambda p: p.read_text().split("\n", 2)[2]  # past the file names
        self.assertEqual(body(a), body(b))
        self.assertIn("+   (recovery 0.3 (range 0.01 1)", body(a))


if __name__ == "__main__":
    unittest.main()
