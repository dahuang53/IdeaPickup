"""Run the fresh-install relay or issue a local first-owner initialization ticket."""
from __future__ import annotations
import argparse
import logging
import os
from pathlib import Path
import sys

from .admin import AdminStore, AdminError
from .identity import IdentityStore, normalize_base_url
from .control import COMMANDS, ControlServer, call, ControlError
from .jobs import RUNTIME_SCHEMA_VERSION
from . import __version__
from config_broker import BrokerClient, BrokerError


class RedactedErrors(logging.Filter):
    def filter(self, record):
        if record.exc_info:
            record.msg, record.args, record.exc_info = "relay request failed", (), None
        return True


def serve(args, env):
    if args.workers != 1 or sys.platform != "linux" or os.geteuid() == 0:
        raise ValueError("one non-root Linux worker is required")
    import uvicorn
    from .runtime import Runtime
    from .web import create_app
    public_url = normalize_base_url(env.get("VOICE_MEMO_PUBLIC_URL", ""))
    database = Path(env.get("VOICE_MEMO_DB", "/var/lib/voice-memo/relay.db"))
    broker = BrokerClient(env.get("VOICE_MEMO_BROKER_SOCKET", "/run/voice-memo/config.sock"))
    runtime = Runtime(database, broker, env.get("VOICE_MEMO_WORK_DIR", "/var/lib/voice-memo-work"),
                      env.get("VOICE_MEMO_STREAM_DIR", "/var/lib/voice-memo/streams"))
    control = None
    logger = logging.getLogger("uvicorn.error"); scrub = RedactedErrors(); logger.addFilter(scrub)
    try:
        app = create_app(database, public_url=public_url, broker=broker, runtime=runtime)
        control = ControlServer(Path(env.get("VOICE_MEMO_CONTROL_SOCKET", "/run/voice-memo-control/control.sock")), runtime,
                                {"code_version": __version__, "schema_version": RUNTIME_SCHEMA_VERSION,
                                 "instance_uuid": runtime.instance_uuid})
        uvicorn.run(app, host=args.host, port=args.port, workers=1, proxy_headers=True,
                    forwarded_allow_ips=env.get("VOICE_MEMO_FORWARDED_ALLOW_IPS", "127.0.0.1"),
                    access_log=False, server_header=False, limit_concurrency=32,
                    timeout_graceful_shutdown=90, timeout_keep_alive=5)
    finally:
        try:
            if control: control.close()
        finally:
            try: runtime.close(timeout=90)
            finally: logger.removeFilter(scrub)


def main(argv=None, env=None):
    env = os.environ if env is None else env
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    server = commands.add_parser("serve")
    server.add_argument("--host", default="127.0.0.1")
    server.add_argument("--port", type=int, default=8765)
    server.add_argument("--workers", type=int, default=1)
    for command in ("bootstrap", "reset"):
        option = commands.add_parser(command)
        option.add_argument("--database", type=Path)
    control = commands.add_parser("control")
    control.add_argument("operation", choices=sorted(COMMANDS))
    control.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.command in {"bootstrap", "reset"}:
            database = args.database or Path(env.get("VOICE_MEMO_DB", "/var/lib/voice-memo/relay.db"))
            IdentityStore.validate_existing(database)
            code = AdminStore(database).issue_bootstrap(reset=args.command == "reset")
            # Caller must redirect this one-time secret into a private file.
            print(code)
        elif args.command == "serve":
            serve(args, env)
        else:
            import json
            value = call(Path(env.get("VOICE_MEMO_CONTROL_SOCKET", "/run/voice-memo-control/control.sock")), args.operation)
            print(json.dumps(value, ensure_ascii=False))
        return 0
    except (AdminError, BrokerError, ControlError, OSError, ValueError, RuntimeError):
        print("Relay command failed; check deployment and private state.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
