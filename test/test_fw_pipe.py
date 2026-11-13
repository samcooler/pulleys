"""The bridge's double-buffered chunk fetch: order of requests and hand-offs, fallbacks, and failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ChunkPipeTests(unittest.TestCase):
    def test_native_chunk_pipe(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "fw-pipe-core")
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
                            str(ROOT / "test/fw_pipe_core.cpp"), "-o", binary], check=True, capture_output=True)
            subprocess.run([binary], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
