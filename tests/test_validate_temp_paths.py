import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRATCH = Path(os.environ.get("COMMANDCODE_SCRATCHPAD") or tempfile.gettempdir())


class ValidationTemporaryPaths(unittest.TestCase):
    def check_failure_cleanup(self, mode, use_scratch):
        with tempfile.TemporaryDirectory(prefix="validate-routing-", dir=SCRATCH) as temporary:
            base = Path(temporary)
            destination = base / "temporary outputs"
            destination.mkdir()
            sentinel = destination / "unrelated"
            sentinel.write_text("keep", encoding="utf-8")
            binaries = base / "bin"
            binaries.mkdir()
            trace = base / "mktemp-args"
            compiler_trace = base / "compiler-args"
            fake_python = binaries / "python3"
            fake_python.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            fake_compiler = binaries / "compiler"
            fake_compiler.write_text(
                '#!/bin/sh\nprintf "%s\\n" "$@" > "$COMPILER_TRACE"\nexit 37\n',
                encoding="utf-8",
            )
            fake_idf = binaries / "idf.py"
            fake_idf.write_text(fake_compiler.read_text(encoding="utf-8"), encoding="utf-8")
            real_mktemp = shutil.which("mktemp")
            self.assertIsNotNone(real_mktemp)
            fake_mktemp = binaries / "mktemp"
            fake_mktemp.write_text(
                '#!/bin/sh\nprintf "%s\\n" "$@" >> "$MKTEMP_TRACE"\n'
                'case "$*" in\n'
                '  *ai-passport-host-tests*) prefix=ai-passport-host-tests ;;\n'
                '  *ai-passport-firmware*) prefix=ai-passport-firmware ;;\n'
                '  *) exit 38 ;;\n'
                'esac\n'
                'exec "$REAL_MKTEMP" -d "$ROUTE_ROOT/$prefix.XXXXXX"\n',
                encoding="utf-8",
            )
            for executable in (fake_python, fake_compiler, fake_idf, fake_mktemp):
                executable.chmod(0o700)
            environment = dict(os.environ)
            environment.update(
                PATH=str(binaries) + os.pathsep + os.environ.get("PATH", ""),
                CC=str(fake_compiler),
                ACTIONLINT_BIN=shutil.which("true") or "/usr/bin/true",
                MKTEMP_TRACE=str(trace),
                COMPILER_TRACE=str(compiler_trace),
                REAL_MKTEMP=real_mktemp,
                ROUTE_ROOT=str(destination),
                TMPDIR=str(destination),
            )
            if use_scratch:
                environment["COMMANDCODE_SCRATCHPAD"] = str(destination)
                environment["TMPDIR"] = str(base)
            else:
                environment.pop("COMMANDCODE_SCRATCHPAD", None)
            result = subprocess.run(
                ["bash", str(ROOT / "tools/validate.sh"), mode],
                cwd=ROOT,
                env=environment,
                text=True,
                capture_output=True,
                timeout=15,
            )
            self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
            arguments = trace.read_text(encoding="utf-8").splitlines()
            requested = arguments[-1]
            self.assertTrue(requested.startswith(str(destination) + "/"), requested)
            leftovers = list(destination.glob("ai-passport-*"))
            self.assertEqual(leftovers, [], str(leftovers))
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep")
            self.assertTrue(compiler_trace.is_file())

    def test_static_prefers_scratch_and_cleans_failure(self):
        self.check_failure_cleanup("--static", True)

    def test_firmware_prefers_scratch_and_cleans_failure(self):
        self.check_failure_cleanup("--firmware", True)

    def test_static_falls_back_to_tmpdir(self):
        self.check_failure_cleanup("--static", False)

    def test_firmware_falls_back_to_tmpdir(self):
        self.check_failure_cleanup("--firmware", False)


if __name__ == "__main__":
    unittest.main()
