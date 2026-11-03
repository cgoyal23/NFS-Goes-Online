# NFS Goes Online

A systems programming project combining an asynchronous Rust HTTP server, a VSFS-style filesystem, and a concurrent C UDP file server.

This is a new implementation built from the three project points in Chirag Goyal's résumé. It is an educational prototype, with the supported behavior and limits documented below. The Rust HTTP service is independent; the UDP service stores files in the VSFS image. This project does not implement the standard NFS wire protocol or a mountable kernel filesystem.

## Features

| Component | Implementation |
| --- | --- |
| Async Rust web server | Non-blocking TCP sockets, `async`/`await`, a small future executor, bounded connection multiplexing, partial reads/writes and explicit I/O error handling |
| VSFS-style filesystem | POSIX `pread`/`pwrite`, inode and free-block bitmaps, hierarchical directories, image persistence and an exclusive process lock |
| Concurrent UDP file server | Four worker threads, bounded request queue, mutex-protected filesystem operations, duplicate-response replay and graceful SIGINT/SIGTERM shutdown |

## Requirements

- Linux or macOS, a C11 compiler, Make and Python 3.9+.
- Rust 1.70+ with Cargo for the web server. No external Rust crates or Python packages are required.
- Linux CI is configured in `.github/workflows/ci.yml`.

## Build and test

```sh
make                 # C server and filesystem test executable
make rust            # Rust server and Rust unit tests
make test            # Filesystem and network integration tests
```

Run `make rust` before `make test` to include the HTTP concurrency test. Otherwise that test is explicitly skipped. Tests create temporary images and launch servers on loopback ports.

## Run the UDP file service

```sh
./build/udp-server disk.img 9000
```

The server creates a new image if the path does not exist, or opens and validates an existing image. A second server cannot open the same image. It binds only to `127.0.0.1`.

In another terminal:

```sh
python3 udp/client.py mkdir /docs
printf 'Hello from VSFS!\n' > hello.txt
python3 udp/client.py put /docs/hello.txt hello.txt
python3 udp/client.py list /docs
python3 udp/client.py get /docs/hello.txt downloaded.txt
cmp hello.txt downloaded.txt
python3 udp/client.py delete /docs/hello.txt
python3 udp/client.py delete /docs
```

Use `--port 9001` before the command to select another port. Press Ctrl+C in the server terminal to stop accepting packets, drain queued work, join workers and close the image. Files persist after restart.

## Run the Rust web server

```sh
cargo run --manifest-path rust-web/Cargo.toml -- 127.0.0.1:8080
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/stats
```

`/health` returns JSON health status. `/stats` returns the number of connections accepted when that connection was created. Unknown paths return 404; unsupported methods return 405. Each connection serves one GET request and closes. Request bodies, HTTP keep-alive and TLS are unsupported.

The server uses standard-library futures and non-blocking sockets. A single thread polls active futures every 2 ms; it does not use OS readiness notifications such as epoll. It caps active connections at 1024, headers at 8192 bytes, and total connection lifetime at five seconds. This makes the async mechanism inspectable, with polling overhead that is unsuitable for a production web service.

## UDP protocol and retry semantics

Requests are ASCII datagrams:

```text
<id> MKDIR /directory
<id> PUT /file <hex-data>
<id> GET /file
<id> LIST /directory
<id> DELETE /file-or-empty-directory
```

IDs contain 1–32 ASCII letters, digits, underscores or hyphens. Use `-` as the data field for an empty file. Replies start with the ID and `OK` or `ERR`; GET returns hex data and LIST returns newline-separated names (directories end in `/`). Paths are absolute within the image, case sensitive, with printable ASCII names up to 47 bytes. Whitespace, repeated slashes, trailing slashes except root, `.` and `..` are rejected. The largest request accepted is 10,000 bytes.

The replay cache stores up to **256 unique requests per server process**, shared across clients. A request is identified by its client IP, UDP port and ID. An exact retry returns the saved response, including errors; reusing an ID with different contents returns `ID_REUSED`. Once full, new requests return `SESSION_FULL` without executing. Existing retries continue to work. Restart the server to begin a new cache session.

The client retries up to five times using the same socket, ID and bytes. Replay guarantees apply only within a running server session and the same client endpoint. Cache entries are not persisted, so there is no exactly-once guarantee across restart, changed client ports or a crash between a filesystem write and caching its response. Cached GET and LIST retries return their original snapshot. UDP loss and reordering are expected; large datagrams may fragment.

## Filesystem layout and limits

```text
Image
  Metadata: magic/version, inode bitmap, block bitmap, 128 inodes
  Data:     128 blocks × 4096 bytes
```

- The root consumes one inode, leaving at most 127 files/directories.
- Each file uses one data block and holds 0–4096 bytes. Directories are represented by parent/name inode relationships and use no data blocks.
- Overwrites write a free block before updating metadata, leaving the old block available until the new data write succeeds. One spare block remains when all 127 non-root inodes hold files.
- Data and metadata writes use `fsync`. There is no journal or transactional metadata update; power loss or I/O failure can still corrupt an image. Back up important images.
- Disk structs use the host ABI and byte order. Images are intended for the same architecture/build, not portable interchange. This is a VSFS-inspired design rather than a byte-compatible implementation of an external VSFS format.
- Library callers must serialize access. The server holds a coarse filesystem mutex around operations and replay-cache updates. Workers are concurrent, but disk mutations are serialized.
- No permissions, authentication, encryption, symlinks, rename, hard links, sparse files or FUSE mounting are implemented. Use disposable local data.

## Source map

```text
fs/vsfs.h, fs/vsfs.c         Image filesystem API and implementation
udp/server.c                Bounded thread pool and replay cache
udp/client.py               Binary-safe command-line client
rust-web/src/main.rs        Async TCP HTTP server and unit tests
tests/test_vsfs.c           Persistence, allocation and boundary checks
tests/test_servers.py       UDP retries/concurrency and HTTP slow-client test
.github/workflows/ci.yml    Linux build and test workflow
```

## Validation

Locally validated on macOS with warnings treated as errors: C filesystem tests, six network integration tests, and two Rust unit tests passed. Tests cover binary and empty files, image persistence, full-size payloads, directory rules, inode exhaustion/reuse, duplicate mutations, reused IDs, full replay cache behavior, exclusive image access and concurrent clients. The included Linux workflow has not yet been observed running.

Project context from the résumé: August 2026–ongoing; supervisor Prof. Mrinmay Biswas, IIT Kanpur; CEA. No performance figures or production-readiness claims are implied.
