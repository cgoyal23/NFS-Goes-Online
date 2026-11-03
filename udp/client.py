#!/usr/bin/env python3
"""UDP client. Retries reuse the exact request and the same socket endpoint."""
import argparse
import socket
import uuid
from pathlib import Path


def request(port, command, path, data=None):
    request_id = uuid.uuid4().hex
    message = f"{request_id} {command} {path}"
    if data is not None:
        if len(data) > 4096:
            raise ValueError("Files must be at most 4096 bytes")
        message += " " + (data.hex() or "-")
    if not path.startswith("/") or any(c.isspace() for c in path):
        raise ValueError("Use an absolute image path without whitespace")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect(("127.0.0.1", port))
        sock.settimeout(0.5)
        for _ in range(5):
            sock.send(message.encode("ascii"))
            try:
                reply = sock.recv(10001).decode("ascii")
            except socket.timeout:
                continue
            if not reply.startswith(request_id + " "):
                continue
            result = reply[len(request_id) + 1:]
            if not (result == "OK" or result.startswith("OK ") or result.startswith("OK\n")):
                raise RuntimeError(result)
            return result
    raise TimeoutError("No response after five attempts")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("command", choices=["put", "get", "mkdir", "list", "delete"])
    parser.add_argument("path")
    parser.add_argument("local", nargs="?", help="local source for put or destination for get")
    args = parser.parse_args()
    if args.command in ("put", "get") and not args.local:
        parser.error("put and get require a local filename")
    data = Path(args.local).read_bytes() if args.command == "put" else None
    reply = request(args.port, args.command.upper(), args.path, data)
    if args.command == "get":
        payload = reply[3:]
        Path(args.local).write_bytes(b"" if payload == "-" else bytes.fromhex(payload))
    else:
        print(reply)


if __name__ == "__main__":
    main()
