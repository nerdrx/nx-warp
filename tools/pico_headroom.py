#!/usr/bin/env python3
"""Disable expensive motion effects in the NX Warp Pico client."""
import argparse
import json
import shlex
import subprocess
import tempfile
from pathlib import Path

PACKAGE = "org.meumeu.wivrn.nx.warp"
CONFIG = "files/client.json"
DEBUG = {
    "debug.wivrn.nx.motion_mode": "off", "debug.wivrn.nx.motion_ema": "0",
    "debug.wivrn.nx.motion_blur": "0", "debug.wivrn.nx.motion_retain4": "0",
    "debug.wivrn.nx.motion_past": "0", "debug.wivrn.nx.motion_trace": "0",
    "debug.wivrn.reducegpu": "1",
}
PERSISTENT = {
    "motion_smoothing": False, "motion_smoothing_server": False,
    "frame_smoothing": False, "reduce_gpu_load": True, "low_poly": False,
    "fsr": False, "cas_sharpening": False, "ambient_glow": False,
    "atlas_prototype": 0, "preferred_refresh_rate": 90, "fps_divider": 1,
}


def adb(args, serial=None, data=None):
    command = ["adb"] + (["-s", serial] if serial else []) + list(args)
    if args and args[0] == "shell":
        command = ["adb"] + (["-s", serial] if serial else []) + ["shell", shlex.join(args[1:])]
    return subprocess.run(command, input=data, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False)


def text(result):
    output = result.stdout or result.stderr
    return output.decode("utf-8", "replace").strip()


def device_info():
    result = adb(["devices", "-l"])
    if result.returncode:
        raise RuntimeError("adb devices failed: " + text(result))
    ready = [line.split()[0] for line in text(result).splitlines()
             if line and not line.startswith("List of") and len(line.split()) > 1
             and line.split()[1] == "device"]
    if len(ready) != 1:
        raise RuntimeError("need exactly one ready adb device (found %d)" % len(ready))
    serial = ready[0]
    manufacturer_result = adb(["shell", "getprop", "ro.product.manufacturer"], serial)
    model_result = adb(["shell", "getprop", "ro.product.model"], serial)
    if manufacturer_result.returncode or model_result.returncode:
        raise RuntimeError("cannot read Pico manufacturer/model")
    manufacturer = text(manufacturer_result).lower()
    model = text(model_result).lower()
    if "pico" not in manufacturer and "pico" not in model and not model.startswith("a8"):
        raise RuntimeError("ready device is not a Pico (manufacturer=%r model=%r)" %
                           (manufacturer, model))
    return serial, manufacturer, model


def read_remote(serial):
    result = adb(["shell", "run-as", PACKAGE, "cat", CONFIG], serial)
    if result.returncode:
        raise RuntimeError("cannot read private config via run-as: " + text(result))
    try:
        parsed = json.loads(result.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError("private config is not valid JSON: %s" % error) from error
    if not isinstance(parsed, dict):
        raise RuntimeError("private config root must be a JSON object")
    return result.stdout, parsed


def props(serial):
    values = {}
    for key in DEBUG:
        result = adb(["shell", "getprop", key], serial)
        if result.returncode:
            raise RuntimeError("cannot read property %s: %s" % (key, text(result)))
        values[key] = text(result)
    return values


def remote_write(serial, payload):
    temp = CONFIG + ".nx-headroom.tmp"
    command = ["shell", "run-as", PACKAGE, "sh", "-c",
               "cat > %s && mv %s %s" % (temp, temp, CONFIG)]
    result = adb(command, serial, payload)
    if result.returncode:
        raise RuntimeError("atomic private config write failed: " + text(result))
    actual, _ = read_remote(serial)
    if actual != payload:
        raise RuntimeError("private config verification failed")


def backup(directory, serial, info, config, original_props):
    root = Path(tempfile.mkdtemp(prefix="nx-warp-headroom-", dir=directory))
    if config is not None:
        (root / "client.json").write_bytes(config)
    (root / "properties.json").write_text(json.dumps({"serial": serial, "manufacturer": info[0],
        "model": info[1], "properties": original_props}, indent=2) + "\n")
    return root


def apply(economy=False, backup_dir=None, runner=None, properties_only=False):
    global adb
    old_adb = adb
    if runner:
        adb = runner
    try:
        if properties_only and economy:
            raise RuntimeError("--economy requires full config access; omit --properties-only")
        serial, manufacturer, model = device_info()
        original_props = props(serial)
        if properties_only:
            stop = adb(["shell", "am", "force-stop", PACKAGE], serial)
            if stop.returncode:
                raise RuntimeError("cannot stop app: " + text(stop))
            root = backup(backup_dir, serial, (manufacturer, model), None, original_props)
            try:
                for key, value in DEBUG.items():
                    result = adb(["shell", "setprop", key, value], serial)
                    if result.returncode:
                        raise RuntimeError("cannot set %s: %s" % (key, text(result)))
                    check = adb(["shell", "getprop", key], serial)
                    if check.returncode or text(check) != value:
                        raise RuntimeError("property verification failed: %s" % key)
            except RuntimeError as error:
                raise RuntimeError("%s (backup: %s)" % (error, root)) from error
            return root
        read_remote(serial)  # Preflight private access before stopping app.
        stop = adb(["shell", "am", "force-stop", PACKAGE], serial)
        if stop.returncode:
            raise RuntimeError("cannot stop app: " + text(stop))
        final_bytes, current = read_remote(serial)
        root = backup(backup_dir, serial, (manufacturer, model), final_bytes, original_props)
        current.update(PERSISTENT)
        if economy:
            current["fps_divider"] = 2
        payload = (json.dumps(current, indent=2, ensure_ascii=False) + "\n").encode()
        try:
            remote_write(serial, payload)
            for key, value in DEBUG.items():
                result = adb(["shell", "setprop", key, value], serial)
                if result.returncode:
                    raise RuntimeError("cannot set %s: %s" % (key, text(result)))
                check = adb(["shell", "getprop", key], serial)
                if check.returncode or text(check) != value:
                    raise RuntimeError("property verification failed: %s" % key)
        except RuntimeError as error:
            raise RuntimeError("%s (backup: %s)" % (error, root)) from error
        return root
    finally:
        adb = old_adb


def restore(path, runner=None):
    global adb
    old_adb = adb
    if runner:
        adb = runner
    try:
        serial, manufacturer, model = device_info()
        metadata = json.loads((Path(path) / "properties.json").read_text())
        if metadata.get("serial") != serial or metadata.get("manufacturer", "").lower() != manufacturer or metadata.get("model", "").lower() != model:
            raise RuntimeError("backup belongs to different device")
        config_path = Path(path) / "client.json"
        config = config_path.read_bytes() if config_path.exists() else None
        if config is not None:
            if not isinstance(json.loads(config), dict):
                raise RuntimeError("backup config root must be a JSON object")
        saved_props = metadata.get("properties")
        if (not isinstance(saved_props, dict) or set(saved_props) != set(DEBUG)
                or any(not isinstance(value, str) for value in saved_props.values())):
            raise RuntimeError("backup properties are incomplete or malformed")
        stop = adb(["shell", "am", "force-stop", PACKAGE], serial)
        if stop.returncode:
            raise RuntimeError("cannot stop app: " + text(stop))
        if config is not None:
            remote_write(serial, config)
        for key, value in saved_props.items():
            result = adb(["shell", "setprop", key, value], serial)
            if result.returncode:
                raise RuntimeError("cannot restore %s: %s" % (key, text(result)))
            check = adb(["shell", "getprop", key], serial)
            if check.returncode or text(check) != value:
                raise RuntimeError("property verification failed: %s" % key)
    finally:
        adb = old_adb


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="apply full config; needs debuggable build")
    parser.add_argument("--properties-only", action="store_true", help="apply runtime debug properties only")
    parser.add_argument("--economy", action="store_true")
    parser.add_argument("--backup-dir")
    parser.add_argument("--restore", metavar="BACKUP")
    args = parser.parse_args(argv)
    try:
        if args.restore and (args.apply or args.properties_only):
            parser.error("--restore cannot be combined with --apply or --properties-only")
        if args.restore:
            restore(args.restore)
            print("restored backup")
        elif args.apply:
            print("backup: %s" % apply(args.economy, args.backup_dir, properties_only=args.properties_only))
        elif args.properties_only:
            parser.error("--properties-only requires --apply")
        else:
            print(json.dumps({"persistent": {**PERSISTENT,
                **({"fps_divider": 2} if args.economy else {})}, "debug": DEBUG}, indent=2))
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        parser.exit(1, "error: %s\n" % error)


if __name__ == "__main__":
    main()
