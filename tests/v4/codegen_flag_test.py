import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CIDER = Path(os.environ.get("CIDER_BIN", REPO / "build" / "v3" / "cider"))
SOURCE = """
from std.io import exit;
func _start() -> void { exit(main()); }
func main() -> int { return 7; }
"""


class CodegenFlagTest(unittest.TestCase):
    def test_fast_and_legacy_are_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "main.cid"
            source.write_text(SOURCE)
            env = dict(os.environ, CIDER_LIB=str(REPO / "modules"))
            for kind in ("legacy", "fast"):
                out = Path(tmp) / (kind + ".asm")
                proc = subprocess.run(
                    [str(CIDER), "compile", "--codegen=" + kind,
                     "-o", str(out), str(source)],
                    capture_output=True, text=True, env=env, timeout=60)
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertIn("_start", out.read_text())

    def test_unknown_codegen_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "main.cid"
            source.write_text(SOURCE)
            proc = subprocess.run(
                [str(CIDER), "compile", "--codegen=wat", "-o",
                 str(Path(tmp) / "x.asm"), str(source)],
                capture_output=True, text=True, timeout=60)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("unknown codegen", proc.stderr)


if __name__ == "__main__":
    unittest.main()
