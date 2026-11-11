"""Synthetic registration-shape reference; no network entry point."""
import json
import uuid
GENERIC_DEVICE_TYPE = "A1MPSLFC7L5AFK"

def registration():
    serial = uuid.uuid4().hex
    return {"domain": "Device", "device_name": "LunaNX Switch " + serial,
            "app_name": "Amazon Luna", "app_version": "1.0.37340.0",
            "device_model": "Nintendo Switch (LunaNX)", "device_type": GENERIC_DEVICE_TYPE,
            "device_serial": serial, "os_version": "Horizon"}


def summarize(status, body):
    summary = {"httpStatus": status, "codePairIssued": False}
    try:
        value = json.loads(body)
        if not isinstance(value, dict):
            return summary
        summary["codePairIssued"] = status == 200 and all(
            isinstance(value.get(key), str) and 0 < len(value[key]) <= 4096
            for key in ("public_code", "private_code")) and all(
                str(value.get(key, "")).isascii() and str(value.get(key, "")).isdigit() and
                0 < int(value[key]) <= 3600
                for key in ("expires_in", "polling_interval_in_seconds"))
        # Only a fixed error classification may leave this function.
        known = {"InvalidValue", "MissingValue", "invalid_request", "invalid_client", "unauthorized_client"}
        error = value.get("error")
        if isinstance(error, str) and error in known:
            summary["errorCategory"] = error
    except (ValueError, TypeError, KeyError, OverflowError):
        pass
    return summary


