import contextlib
import io
import json
import tempfile
import unittest
import shlex
from pathlib import Path
from unittest.mock import Mock, patch

import pico_headroom as tool


class FakeAdb:
    def __init__(self, config):
        self.config = config
        self.calls = []
        self.props = {key: "old" for key in tool.DEBUG}

    def __call__(self, args, serial=None, data=None):
        self.calls.append((args, serial, data))
        out = b""
        command = (shlex.split(args[1]) if len(args) == 2 else args[1:]) if args and args[0] == "shell" else args
        if command[:2] == ["devices", "-l"]:
            out = b"List of devices attached\npico device product:x model:PICO_4 device\n"
        elif command[-2:] == ["getprop", "ro.product.manufacturer"]:
            out = b"PICO"
        elif command[-2:] == ["getprop", "ro.product.model"]:
            out = b"PICO_4"
        elif command[0:4] == ["run-as", tool.PACKAGE, "cat", tool.CONFIG]:
            out = self.config
        elif command[0:3] == ["run-as", tool.PACKAGE, "sh"]:
            self.config = data
        elif command[-2] == "getprop":
            out = self.props.get(command[-1], "").encode()
        elif command[-3] == "setprop":
            self.props[command[-2]] = command[-1]
        return Mock(returncode=0, stdout=out, stderr=b"")


class HeadroomTests(unittest.TestCase):
    def test_default_has_no_adb_side_effect(self):
        with patch.object(tool, "adb", side_effect=AssertionError("unexpected ADB")), contextlib.redirect_stdout(io.StringIO()) as output:
            tool.main([])
        self.assertIn("persistent", output.getvalue())

    def test_apply_preserves_unknown_and_restore(self):
        source = json.dumps({"servers": [1], "unknown": {"x": 2}, "resolution": 72}).encode()
        fake = FakeAdb(source)
        with tempfile.TemporaryDirectory() as tmp:
            root = tool.apply(backup_dir=tmp, runner=fake)
            written = next(call[2] for call in fake.calls if call[2] and b"unknown" in call[2])
            result = json.loads(written)
            self.assertEqual(result["unknown"], {"x": 2})
            self.assertEqual(result["servers"], [1])
            tool.restore(root, runner=fake)
            self.assertEqual((Path(root) / "client.json").read_bytes(), source)
            self.assertEqual(fake.config, source)

    def test_unavailable_run_as_does_not_write(self):
        fake = FakeAdb(b"{}")
        def fail_read(args, serial=None, data=None):
            result = fake(args, serial, data)
            if args[0:4] == ["shell", "run-as", tool.PACKAGE, "cat"]:
                result.returncode = 1
                result.stderr = b"package unavailable"
            return result
        with self.assertRaisesRegex(RuntimeError, "cannot read"):
            tool.apply(runner=fail_read)
        self.assertFalse(any(call[2] for call in fake.calls))
        self.assertFalse(any("force-stop" in call[0] or "setprop" in call[0] for call in fake.calls))

    def test_properties_only_works_when_run_as_unavailable_and_restores_empty(self):
        fake = FakeAdb(b"{}"); fake.props = {key: "" for key in tool.DEBUG}
        with tempfile.TemporaryDirectory() as tmp:
            root = tool.apply(backup_dir=tmp, runner=fake, properties_only=True)
            self.assertFalse((Path(root) / "client.json").exists())
            self.assertEqual(json.loads((Path(root) / "properties.json").read_text())["properties"]["debug.wivrn.nx.motion_mode"], "")
            tool.restore(root, runner=fake)
            self.assertEqual(fake.props["debug.wivrn.nx.motion_mode"], "")
            self.assertFalse(any("run-as" in call[0] for call in fake.calls))
            self.assertEqual(fake.config, b"{}")

    def test_properties_only_rejects_economy(self):
        with self.assertRaisesRegex(RuntimeError, "economy"):
            tool.apply(economy=True, runner=Mock(side_effect=AssertionError("unexpected ADB")), properties_only=True)

    def test_real_adb_wrapper_preserves_shell_arguments(self):
        with patch.object(tool.subprocess, "run") as run:
            tool.adb(["shell", "setprop", "debug.wivrn.nx.motion_mode", ""], "pico")
            argv = run.call_args.args[0]
            self.assertEqual(argv[:4], ["adb", "-s", "pico", "shell"])
            self.assertEqual(shlex.split(argv[4]), ["setprop", "debug.wivrn.nx.motion_mode", ""])
            script = "cat > files/client.json.tmp && mv files/client.json.tmp files/client.json"
            tool.adb(["shell", "run-as", tool.PACKAGE, "sh", "-c", script], "pico", b"{}")
            self.assertEqual(shlex.split(run.call_args.args[0][4]), ["run-as", tool.PACKAGE, "sh", "-c", script])
            self.assertEqual(run.call_args.kwargs["input"], b"{}")

    def test_restore_rejects_malformed_properties_before_stop(self):
        fake = FakeAdb(b"{}")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "properties.json").write_text(json.dumps({"serial": "pico", "manufacturer": "pico",
                "model": "pico_4", "properties": {}}))
            with self.assertRaisesRegex(RuntimeError, "incomplete"):
                tool.restore(root, runner=fake)
        self.assertFalse(any(call[0][0] == "shell" and "force-stop" in str(call[0]) for call in fake.calls))


if __name__ == "__main__":
    unittest.main()
