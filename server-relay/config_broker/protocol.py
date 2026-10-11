"""Explicit RPC allowlist. No dynamic module, method, or filename resolution."""
from .core import BrokerError

VERSION = 1
MAX_FRAME = 131072
METHOD_PARAMS = {
    "view": (),
    "validate": ("patch",),
    "save_activate": ("expected_revision", "patch"),
    "pin_active": ("task_id",),
    "pin_existing": ("task_id", "revision"),
    "read_snapshot": ("revision", "task_id"),
    "release": ("task_id",),
    "begin_maintenance": (),
    "reconcile": ("generation", "references"),
    "end_maintenance": ("generation",),
    "collect": (),
}


def dispatch(broker, request):
    identifier = None
    try:
        if not isinstance(request, dict) or set(request) != {"version", "id", "method", "params"}:
            raise BrokerError("invalid_request")
        identifier = request["id"]
        if not isinstance(identifier, str) or len(identifier) > 64 or not identifier.isascii() or not all(32 <= ord(c) < 127 for c in identifier):
            identifier = None
            raise BrokerError("invalid_request")
        if type(request["version"]) is not int or request["version"] != VERSION:
            raise BrokerError("unsupported_version")
        method, params = request["method"], request["params"]
        if not isinstance(method, str) or method not in METHOD_PARAMS:
            raise BrokerError("unknown_method")
        if not isinstance(params, dict) or set(params) != set(METHOD_PARAMS[method]):
            raise BrokerError("invalid_params")
        # getattr is deliberately restricted to this literal method table.
        result = getattr(broker, method)(**params)
        return {"version": VERSION, "id": identifier, "ok": True, "result": result}
    except BrokerError as exc:
        return {"version": VERSION, "id": identifier, "ok": False, "error": {"code": exc.code}}
    except (OSError, ValueError, TypeError, KeyError):
        # Neither input values, filenames, exception text nor secret data escape.
        return {"version": VERSION, "id": identifier, "ok": False, "error": {"code": "storage_failure"}}
