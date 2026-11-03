import concurrent.futures
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class UDPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.port = free_port(socket.SOCK_DGRAM)
        self.image = str(Path(self.temp.name) / "disk.img")
        self.start()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.connect(("127.0.0.1", self.port))
        self.sock.settimeout(2)

    def start(self):
        self.proc = subprocess.Popen([str(ROOT / "build/udp-server"), self.image, str(self.port)], stderr=subprocess.PIPE)
        line = self.proc.stderr.readline()
        self.assertIn(b"UDP file server", line)

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        self.assertEqual(self.proc.wait(timeout=5), 0)
        self.proc.stderr.close()

    def tearDown(self):
        self.sock.close()
        if self.proc.poll() is None:
            self.stop()
        self.temp.cleanup()

    def exchange(self, message, sock=None):
        sock = sock or self.sock
        sock.send(message)
        return sock.recv(10001)

    def test_binary_persistence_and_retry(self):
        payload = bytes(range(256)) * 16
        put = b"p PUT /binary " + payload.hex().encode()
        self.assertEqual(self.exchange(put), b"p OK")
        self.assertEqual(self.exchange(put), b"p OK")
        self.assertIn(b"ID_REUSED", self.exchange(b"p DELETE /binary"))
        self.assertEqual(self.exchange(b"g GET /binary"), b"g OK " + payload.hex().encode())
        self.stop()
        self.start()
        self.assertEqual(self.exchange(b"after GET /binary"), b"after OK " + payload.hex().encode())

    def test_directories_errors_and_delete_retry(self):
        self.assertEqual(self.exchange(b"a MKDIR /docs"), b"a OK")
        self.assertEqual(self.exchange(b"b PUT /docs/file 6869"), b"b OK")
        self.assertEqual(self.exchange(b"c LIST /docs"), b"c OK\nfile\n")
        self.assertIn(b" ERR ", self.exchange(b"d DELETE /docs"))
        self.assertEqual(self.exchange(b"e DELETE /docs/file"), b"e OK")
        self.assertEqual(self.exchange(b"e DELETE /docs/file"), b"e OK")
        self.assertIn(b"INVALID_HEX", self.exchange(b"f PUT /bad zz"))
        self.assertIn(b" ERR ", self.exchange(b"g PUT /../bad 00"))
        self.assertEqual(self.exchange(b"zero PUT /empty -"), b"zero OK")
        self.assertEqual(self.exchange(b"read GET /empty"), b"read OK -")
        self.assertIn(b"INVALID_REQUEST", self.exchange(b"nul GET /\x00"))

    def test_concurrent_clients(self):
        def work(i):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.connect(("127.0.0.1", self.port))
                sock.settimeout(3)
                self.assertEqual(self.exchange(f"put PUT /f{i} 6162".encode(), sock), b"put OK")
                self.assertEqual(self.exchange(f"get GET /f{i}".encode(), sock), b"get OK 6162")
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            list(pool.map(work, range(40)))

    def test_cache_full_never_reexecutes(self):
        for i in range(256):
            self.assertEqual(self.exchange(f"r{i} LIST /".encode()), f"r{i} OK\n".encode())
        self.assertEqual(self.exchange(b"overflow MKDIR /blocked"), b"overflow ERR SESSION_FULL")
        self.assertEqual(self.exchange(b"r0 LIST /"), b"r0 OK\n")

    def test_image_exclusive_across_processes(self):
        other = subprocess.run([str(ROOT / "build/udp-server"), self.image, str(free_port(socket.SOCK_DGRAM))], capture_output=True, timeout=3)
        self.assertNotEqual(other.returncode, 0)


RUST_BINARY = ROOT / "rust-web/target/debug/nfs-web"


@unittest.skipUnless(RUST_BINARY.exists(), "Build Rust server with make rust first")
class HTTPTests(unittest.TestCase):
    def test_slow_client_does_not_block_others(self):
        port = free_port(socket.SOCK_STREAM)
        proc = subprocess.Popen([str(RUST_BINARY), f"127.0.0.1:{port}"], stderr=subprocess.PIPE)
        slow = None
        try:
            self.assertIn(b"Rust HTTP server", proc.stderr.readline())
            slow = socket.create_connection(("127.0.0.1", port), timeout=2)
            slow.sendall(b"GET /health HTTP/1.1\r\nHost:")
            def fetch(_):
                with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                    sock.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    result = b""
                    while True:
                        chunk = sock.recv(4096)
                        if not chunk:
                            break
                        result += chunk
                    self.assertIn(b"200 OK", result)
                    head, body = result.split(b"\r\n\r\n", 1)
                    self.assertIn(f"Content-Length: {len(body)}".encode(), head)
            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
                list(pool.map(fetch, range(40)))
            slow.sendall(b" localhost\r\n\r\n")
            self.assertIn(b"200 OK", slow.recv(4096))
        finally:
            if slow:
                slow.close()
            proc.terminate()
            proc.wait(timeout=3)
            proc.stderr.close()


if __name__ == "__main__":
    unittest.main()
