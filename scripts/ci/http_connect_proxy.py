#!/usr/bin/env python3

import base64
import os
import select
import socket
import threading

BIND_HOST = "127.0.0.1"
BIND_PORT = int(os.environ["IT_HTTP_PROXY_PORT"])
AUTH_USER = os.environ.get("IT_HTTP_PROXY_USER", "")
AUTH_PASS = os.environ.get("IT_HTTP_PROXY_PASS", "")
EXPECTED_AUTH = "Basic " + base64.b64encode(
    f"{AUTH_USER}:{AUTH_PASS}".encode("utf-8")
).decode("ascii")


def parse_authority(authority: str):
    if authority.startswith("["):
        end = authority.find("]")
        if end <= 0:
            raise ValueError("invalid IPv6 authority")
        host = authority[1:end]
        rest = authority[end + 1 :]
        if not rest.startswith(":"):
            raise ValueError("missing port")
        return host, int(rest[1:])
    host, port = authority.rsplit(":", 1)
    return host, int(port)


def tunnel(client: socket.socket, upstream: socket.socket):
    sockets = [client, upstream]
    while True:
        readable, _, _ = select.select(sockets, [], [], 30.0)
        if not readable:
            continue
        for source in readable:
            data = source.recv(65536)
            if not data:
                return
            destination = upstream if source is client else client
            destination.sendall(data)


def handle(client: socket.socket):
    upstream = None
    try:
        client.settimeout(10.0)
        reader = client.makefile("rb")
        request_line = reader.readline(8192).decode(
            "iso-8859-1", errors="replace"
        ).strip()
        if not request_line:
            return
        parts = request_line.split()
        if len(parts) < 3 or parts[0].upper() != "CONNECT":
            client.sendall(
                b"HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n"
            )
            return
        authority = parts[1]
        headers = {}
        while True:
            line = reader.readline(8192)
            if not line or line in (b"\r\n", b"\n"):
                break
            decoded = line.decode("iso-8859-1", errors="replace").strip()
            if not decoded or ":" not in decoded:
                continue
            key, value = decoded.split(":", 1)
            headers[key.strip().lower()] = value.strip()
        if headers.get("proxy-authorization", "") != EXPECTED_AUTH:
            client.sendall(
                b"HTTP/1.1 407 Proxy Authentication Required\r\n"
                b"Proxy-Authenticate: Basic realm=\"OpenSCP IT\"\r\n"
                b"Connection: close\r\n\r\n"
            )
            return
        host, port = parse_authority(authority)
        upstream = socket.create_connection((host, port), timeout=10.0)
        client.sendall(
            b"HTTP/1.1 200 Connection Established\r\n"
            b"Connection: keep-alive\r\n\r\n"
        )
        client.settimeout(None)
        upstream.settimeout(None)
        tunnel(client, upstream)
    except Exception:
        try:
            client.sendall(
                b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n"
            )
        except Exception:
            pass
    finally:
        try:
            if upstream is not None:
                upstream.close()
        except Exception:
            pass
        try:
            client.close()
        except Exception:
            pass


def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((BIND_HOST, BIND_PORT))
    server.listen(64)
    while True:
        client, _ = server.accept()
        thread = threading.Thread(target=handle, args=(client,), daemon=True)
        thread.start()


if __name__ == "__main__":
    main()
