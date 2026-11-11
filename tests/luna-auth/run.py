#!/usr/bin/env python3
"""Offline native-login tests. Never contacts Amazon or uses account data."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("preflight", ROOT / "tests/luna-auth/registration_fixture.py")
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


def main():
    payload = {"public_code": "ABCD1234", "private_code": "FAKE_PRIVATE_SECRET",
               "access_token": "FAKE_ACCESS_SECRET", "expires_in": 600, "polling_interval_in_seconds": 1}
    assert preflight.summarize(200, json.dumps(payload)) == {"httpStatus": 200, "codePairIssued": True}
    for status, body in [(400, payload), (200, {}), (200, []), (200, dict(payload, private_code=None)),
                         (200, dict(payload, expires_in=True)), (200, dict(payload, expires_in=0)),
                         (400, {"error": "FAKE_PRIVATE_SECRET", "error_description": "FAKE_ACCESS_SECRET"})]:
        summary = preflight.summarize(status, json.dumps(body))
        assert not summary["codePairIssued"] and "FAKE_" not in json.dumps(summary)
    assert preflight.summarize(400, '{"error":"InvalidValue"}')["errorCategory"] == "InvalidValue"
    first, second = preflight.registration(), preflight.registration()
    assert first["device_serial"] != second["device_serial"]
    assert first["device_model"] == "Nintendo Switch (LunaNX)"
    assert first["device_name"] != second["device_name"]
    for item in (first, second):
        assert item["device_name"] == "LunaNX Switch " + item["device_serial"]
        assert len(item["device_name"]) <= 64
    with tempfile.TemporaryDirectory(prefix="lunanx-auth-") as temporary:
        binary = str(Path(temporary) / "auth-unit")
        subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-Isource", "-Iexternal/libdatachannel/deps/json/include", "source/native_auth.cpp",
                        "source/native_session.cpp", "source/native_profiles.cpp", "source/luna_session.cpp", "tests/luna-auth/unit.cpp", "-o", binary], cwd=ROOT, check=True)
        subprocess.run([binary], check=True)
    print("PASS: registration preflight summaries omit codes, tokens, response bodies and device serials.")


if __name__ == "__main__":
    main()
