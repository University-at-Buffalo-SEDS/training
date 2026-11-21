from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AcquisitionTests(unittest.TestCase):
    def test_drivers_conversion_and_storage_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "acquisition-tests"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", "-Itests/sensor_stubs", "-ICore/Inc",
                "tests/test_acquisition.c", "Core/Src/spi_bus.c", "Core/Src/sensors.c",
                "Core/Src/records.c", "Core/Src/acquisition.c", "Core/Src/sensor_live.c",
                "Core/Src/sensor_thread.c", "Core/Src/processing_thread.c", "-lm", "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
