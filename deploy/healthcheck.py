"""Local liveness checks with bounded output, never external API calls."""
import json
import socket
import sys
import urllib.request


def main():
    if sys.argv[1:] == ["relay"]:
        with urllib.request.urlopen("http://127.0.0.1:8765/health/live", timeout=3) as response:
            value = json.load(response)
            assert response.status == 200 and value["status"] == "ok"
    elif sys.argv[1:] == ["broker"]:
        # The root health-check UID is intentionally not the permitted relay UID.
        # An explicit peer rejection proves the live broker is enforcing its gate.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(3)
            connection.connect("/run/voice-memo/config.sock")
            value = json.loads(connection.recv(1024))
            assert value == {"version": 1, "id": None, "ok": False,
                             "error": {"code": "unauthorized_peer"}}
    else:
        raise ValueError("invalid probe")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        raise SystemExit(1) from None
