//! Dependency-free teaching reactor: futures poll non-blocking TCP sockets.
use std::future::{poll_fn, Future};
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll, Wake, Waker};
use std::time::{Duration, Instant};

const MAX_HEADER: usize = 8192;
const MAX_CONNECTIONS: usize = 1024;
struct NoopWake;
impl Wake for NoopWake { fn wake(self: Arc<Self>) {} }
type Task = Pin<Box<dyn Future<Output = io::Result<()>>>>;
struct Connection { task: Task, deadline: Instant }

async fn read_some(stream: &mut TcpStream, bytes: &mut [u8]) -> io::Result<usize> {
    poll_fn(|_| match stream.read(bytes) {
        Err(e) if e.kind() == io::ErrorKind::WouldBlock => Poll::Pending,
        Err(e) if e.kind() == io::ErrorKind::Interrupted => Poll::Pending,
        result => Poll::Ready(result),
    }).await
}
async fn write_all(stream: &mut TcpStream, mut bytes: &[u8]) -> io::Result<()> {
    while !bytes.is_empty() {
        let n = poll_fn(|_| match stream.write(bytes) {
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => Poll::Pending,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => Poll::Pending,
            result => Poll::Ready(result),
        }).await?;
        if n == 0 { return Err(io::Error::new(io::ErrorKind::WriteZero, "closed socket")); }
        bytes = &bytes[n..];
    }
    Ok(())
}
fn response(status: &str, body: &str) -> Vec<u8> {
    format!("HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len()).into_bytes()
}
fn route(header: &[u8], accepted: u64) -> Vec<u8> {
    let text = match std::str::from_utf8(header) {
        Ok(text) => text,
        Err(_) => return response("400 Bad Request", "{\"error\":\"invalid encoding\"}\n"),
    };
    let mut lines = text.split("\r\n");
    let first = lines.next().unwrap_or("");
    let parts: Vec<_> = first.split(' ').collect();
    if parts.len() != 3 || !["HTTP/1.0", "HTTP/1.1"].contains(&parts[2]) || !parts[1].starts_with('/') {
        return response("400 Bad Request", "{\"error\":\"invalid request line\"}\n");
    }
    let mut hosts = 0;
    for line in lines.take_while(|line| !line.is_empty()) {
        let (name, value) = match line.split_once(':') {
            Some(pair) => pair,
            None => return response("400 Bad Request", "{\"error\":\"invalid header\"}\n"),
        };
        if name.is_empty() || !name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-') || value.bytes().any(|b| b < 32 && b != b'\t') {
            return response("400 Bad Request", "{\"error\":\"invalid header\"}\n");
        }
        if name.eq_ignore_ascii_case("host") { hosts += 1; if value.trim().is_empty() { hosts += 2; } }
        if name.eq_ignore_ascii_case("transfer-encoding") || (name.eq_ignore_ascii_case("content-length") && value.trim() != "0") {
            return response("400 Bad Request", "{\"error\":\"request bodies unsupported\"}\n");
        }
    }
    if (parts[2] == "HTTP/1.1" && hosts != 1) || hosts > 1 {
        return response("400 Bad Request", "{\"error\":\"invalid host\"}\n");
    }
    if parts[0] != "GET" { return response("405 Method Not Allowed", "{\"error\":\"GET only\"}\n"); }
    match parts[1] {
        "/" => response("200 OK", "{\"project\":\"NFS Goes Online\",\"service\":\"async Rust web server\"}\n"),
        "/health" => response("200 OK", "{\"status\":\"ok\"}\n"),
        "/stats" => response("200 OK", &format!("{{\"accepted_at_connection\":{accepted}}}\n")),
        _ => response("404 Not Found", "{\"error\":\"not found\"}\n"),
    }
}
async fn serve(mut stream: TcpStream, accepted: u64) -> io::Result<()> {
    let mut header = Vec::new();
    let mut buffer = [0u8; 1024];
    let reply = loop {
        let n = read_some(&mut stream, &mut buffer).await?;
        if n == 0 { return Ok(()); }
        header.extend_from_slice(&buffer[..n]);
        if let Some(end) = header.windows(4).position(|w| w == b"\r\n\r\n") {
            if end + 4 > MAX_HEADER { break response("431 Request Header Fields Too Large", "{\"error\":\"header too large\"}\n"); }
            break route(&header[..end+4], accepted);
        }
        if header.len() >= MAX_HEADER { break response("431 Request Header Fields Too Large", "{\"error\":\"header too large\"}\n"); }
    };
    write_all(&mut stream, &reply).await
}
fn main() -> io::Result<()> {
    let address = std::env::args().nth(1).unwrap_or_else(|| "127.0.0.1:8080".to_string());
    let listener = TcpListener::bind(&address)?;
    listener.set_nonblocking(true)?;
    eprintln!("Rust HTTP server on {}", listener.local_addr()?);
    let waker: Waker = Waker::from(Arc::new(NoopWake));
    let mut context = Context::from_waker(&waker);
    let mut tasks: Vec<Connection> = Vec::new();
    let mut accepted = 0u64;
    loop {
        // Bound accepts per tick so a connection flood cannot starve existing tasks.
        for _ in 0..64 {
            match listener.accept() {
                Ok((stream, _)) => {
                    if tasks.len() >= MAX_CONNECTIONS { drop(stream); continue; }
                    if let Err(error) = stream.set_nonblocking(true) { eprintln!("socket: {error}"); continue; }
                    accepted = accepted.saturating_add(1);
                    tasks.push(Connection { task: Box::pin(serve(stream, accepted)), deadline: Instant::now() + Duration::from_secs(5) });
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) => { eprintln!("accept: {e}"); break; }
            }
        }
        let mut i = 0;
        while i < tasks.len() {
            let expired = Instant::now() >= tasks[i].deadline;
            let done = if expired { true } else {
                match tasks[i].task.as_mut().poll(&mut context) {
                    Poll::Ready(Err(error)) => { eprintln!("connection: {error}"); true }
                    Poll::Ready(Ok(())) => true,
                    Poll::Pending => false,
                }
            };
            if done { drop(tasks.swap_remove(i)); } else { i += 1; }
        }
        std::thread::sleep(Duration::from_millis(2));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn routes_and_errors() {
        for (input, expected) in [
            ("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n", "200 OK"),
            ("GET /missing HTTP/1.0\r\n\r\n", "404 Not Found"),
            ("POST / HTTP/1.0\r\n\r\n", "405 Method Not Allowed"),
            ("GET / HTTP/1.1\r\n\r\n", "400 Bad Request"),
            ("GET / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", "400 Bad Request"),
        ] { assert!(String::from_utf8(route(input.as_bytes(), 1)).unwrap().contains(expected)); }
    }
    #[test]
    fn content_length_is_bytes() {
        let output = String::from_utf8(response("200 OK", "é")).unwrap();
        assert!(output.contains("Content-Length: 2\r\n"));
    }
}
