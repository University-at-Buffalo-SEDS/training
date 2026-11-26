from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StorageTests(unittest.TestCase):
    def test_launch_detection(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "detector-tests"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
                "-ICore/Inc", "tests/test_launch_detector.c", "Core/Src/launch_detector.c", "-lm", "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)

    def test_compact_launch_capture_and_protection(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "launch-tests"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", "-ICore/Inc", "-Itests",
                "tests/test_launch_storage.c", "Core/Src/storage.c", "Core/Src/compact_record.c",
                "Core/Src/records.c", "Core/Src/usb_protocol.c", "-lm", "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)

    def test_w25q_driver_against_spi_device_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "flash-tests"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", "-Itests/sensor_stubs", "-ICore/Inc", "-IDrivers/flash",
                "tests/test_flash_driver.c", "Drivers/flash/w25q32jv.c", "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)

    def test_service_commands_and_boot_logging(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "service-tests"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", "-Itests/sensor_stubs", "-ICore/Inc", "-Itests",
                "tests/test_storage_service.c", "Core/Src/storage.c", "Core/Src/compact_record.c",
                "Core/Src/storage_service.c", "Core/Src/launch_detector.c",
                "Core/Src/records.c", "Core/Src/usb_protocol.c", "Core/Src/usb_service.c",
                "-lm", "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)

    def test_persistence_and_power_loss(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "storage-tests"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", "-ICore/Inc", "-Itests",
                "tests/test_storage.c", "Core/Src/storage.c", "Core/Src/compact_record.c", "Core/Src/records.c",
                "Core/Src/usb_protocol.c", "-lm", "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)
