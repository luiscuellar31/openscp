#!/usr/bin/env python3
"""An SSH server with SFTP for the integration tests of the Windows CI job.

The Linux jobs test against OpenSSH's sshd, which Windows runners lack, so
this paramiko server stands in for it on 127.0.0.1. Remote absolute paths
map to --root. POSIX permission bits set through SFTP are remembered while
the server runs, since Windows files cannot hold them. Symbolic links and
commands, and with them SCP, are refused. With --forward it also opens
direct-tcpip channels, as a jump host does for "ssh -W".

--keys holds an Ed25519 host key and the client key the server accepts,
client_ed25519 with its .pub; both are made when missing. Each connection
offers the host key and an RSA key made at start, as sshd offers several.
"""

import argparse
import errno
import os
import socket
import stat
import sys
import threading
from pathlib import Path

import paramiko
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from paramiko import SFTPAttributes, SFTPHandle, SFTPServer, SFTPServerInterface


def log(line: str) -> None:
    print(line, file=sys.stderr, flush=True)


def ensure_key(path: Path) -> None:
    if path.exists():
        return
    key = Ed25519PrivateKey.generate()
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                       serialization.PrivateFormat.OpenSSH,
                                       serialization.NoEncryption()))
    public = key.public_key().public_bytes(serialization.Encoding.OpenSSH,
                                           serialization.PublicFormat.OpenSSH)
    path.with_name(path.name + ".pub").write_bytes(public + b" openscp-it\n")


class Modes:
    """POSIX permission bits that Windows files cannot store."""

    def __init__(self):
        self.lock = threading.Lock()
        self.bits = {}

    def set(self, path: str, mode: int) -> None:
        with self.lock:
            self.bits[os.path.normcase(path)] = mode & 0o7777

    def apply(self, path: str, attr: SFTPAttributes) -> SFTPAttributes:
        with self.lock:
            bits = self.bits.get(os.path.normcase(path))
        if bits is not None and attr.st_mode is not None:
            attr.st_mode = stat.S_IFMT(attr.st_mode) | bits
        return attr

    def move(self, old: str, new: str) -> None:
        with self.lock:
            old_key, new_key = os.path.normcase(old), os.path.normcase(new)
            for key in [k for k in self.bits if k == old_key or k.startswith(old_key + os.sep)]:
                self.bits[new_key + key[len(old_key):]] = self.bits.pop(key)

    def forget(self, path: str) -> None:
        with self.lock:
            self.bits.pop(os.path.normcase(path), None)


def errno_result(error: OSError) -> int:
    return SFTPServer.convert_errno(error.errno or errno.EIO)


def set_attributes(path: str, attr: SFTPAttributes, modes: Modes) -> None:
    if attr._flags & attr.FLAG_PERMISSIONS:
        modes.set(path, attr.st_mode)
    if attr._flags & attr.FLAG_AMTIME:
        os.utime(path, (attr.st_atime, attr.st_mtime))
    if attr._flags & attr.FLAG_SIZE:
        with open(path, "r+b") as stream:
            stream.truncate(attr.st_size)


def sftp_server_class(root: Path, modes: Modes):
    class Handle(SFTPHandle):
        def stat(self):
            try:
                return modes.apply(self.path,
                                   SFTPAttributes.from_stat(os.fstat(self.readfile.fileno())))
            except OSError as error:
                return errno_result(error)

        def chattr(self, attr):
            try:
                set_attributes(self.path, attr, modes)
                return paramiko.SFTP_OK
            except OSError as error:
                return errno_result(error)

    class Server(SFTPServerInterface):
        def _local(self, path: str) -> str:
            return str(root) + self.canonicalize(path).replace("/", os.sep)

        def _stat(self, path: str, read):
            local = self._local(path)
            try:
                return modes.apply(local, SFTPAttributes.from_stat(read(local)))
            except OSError as error:
                return errno_result(error)

        def list_folder(self, path):
            local = self._local(path)
            try:
                entries = []
                for name in os.listdir(local):
                    full = os.path.join(local, name)
                    attr = modes.apply(full, SFTPAttributes.from_stat(os.lstat(full)))
                    attr.filename = name
                    entries.append(attr)
                return entries
            except OSError as error:
                return errno_result(error)

        def stat(self, path):
            return self._stat(path, os.stat)

        def lstat(self, path):
            return self._stat(path, os.lstat)

        def open(self, path, flags, attr):
            local = self._local(path)
            try:
                fd = os.open(local, flags | getattr(os, "O_BINARY", 0), 0o666)
            except OSError as error:
                return errno_result(error)
            if flags & os.O_CREAT and attr is not None:
                mode = attr.st_mode if attr.st_mode is not None else 0o666
                attr._flags &= ~attr.FLAG_PERMISSIONS
                modes.set(local, mode)
                try:
                    set_attributes(local, attr, modes)
                except OSError:
                    pass
            if flags & os.O_WRONLY:
                file_mode = "ab" if flags & os.O_APPEND else "wb"
            elif flags & os.O_RDWR:
                file_mode = "a+b" if flags & os.O_APPEND else "r+b"
            else:
                file_mode = "rb"
            try:
                stream = os.fdopen(fd, file_mode)
            except OSError as error:
                return errno_result(error)
            handle = Handle(flags)
            handle.path = local
            handle.filename = local
            handle.readfile = stream
            handle.writefile = stream
            return handle

        def remove(self, path):
            local = self._local(path)
            try:
                os.remove(local)
                modes.forget(local)
                return paramiko.SFTP_OK
            except OSError as error:
                return errno_result(error)

        def rename(self, oldpath, newpath):
            old, new = self._local(oldpath), self._local(newpath)
            # SFTP's own rename never replaces, as OpenSSH's sftp-server.
            if os.path.lexists(new):
                return paramiko.SFTP_FAILURE
            try:
                os.rename(old, new)
                modes.move(old, new)
                return paramiko.SFTP_OK
            except OSError as error:
                return errno_result(error)

        def posix_rename(self, oldpath, newpath):
            old, new = self._local(oldpath), self._local(newpath)
            try:
                os.replace(old, new)
                modes.move(old, new)
                return paramiko.SFTP_OK
            except OSError as error:
                return errno_result(error)

        def mkdir(self, path, attr):
            local = self._local(path)
            try:
                os.mkdir(local)
                if attr is not None:
                    set_attributes(local, attr, modes)
                return paramiko.SFTP_OK
            except OSError as error:
                return errno_result(error)

        def rmdir(self, path):
            local = self._local(path)
            try:
                os.rmdir(local)
                modes.forget(local)
                return paramiko.SFTP_OK
            except OSError as error:
                return errno_result(error)

        def chattr(self, path, attr):
            local = self._local(path)
            try:
                set_attributes(local, attr, modes)
                return paramiko.SFTP_OK
            except OSError as error:
                return errno_result(error)

        def symlink(self, target_path, path):
            return paramiko.SFTP_OP_UNSUPPORTED

        def readlink(self, path):
            return paramiko.SFTP_OP_UNSUPPORTED

    return Server


def pump(source, destination) -> None:
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            destination.sendall(data)
    except OSError as error:
        log(f"forwarding stopped: {error}")
    finally:
        try:
            if isinstance(destination, paramiko.Channel):
                # As sshd does: once the forwarded connection ends, the channel
                # closes, and ssh -W exits.
                destination.close()
            else:
                destination.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--keys", type=Path, required=True)
    parser.add_argument("--forward", action="store_true")
    args = parser.parse_args()

    args.root.mkdir(parents=True, exist_ok=True)
    args.keys.mkdir(parents=True, exist_ok=True)
    ensure_key(args.keys / "host_ed25519")
    ensure_key(args.keys / "client_ed25519")
    host_keys = [paramiko.Ed25519Key.from_private_key_file(str(args.keys / "host_ed25519")),
                 paramiko.RSAKey.generate(2048)]
    accepted = (args.keys / "client_ed25519.pub").read_text(encoding="utf-8").split()[:2]
    modes = Modes()
    sftp_server = sftp_server_class(args.root, modes)

    class Server(paramiko.ServerInterface):
        def __init__(self):
            self.forwards = {}

        def get_allowed_auths(self, username):
            return "publickey"

        def check_auth_publickey(self, username, key):
            if [key.get_name(), key.get_base64()] == accepted:
                return paramiko.AUTH_SUCCESSFUL
            return paramiko.AUTH_FAILED

        def check_channel_request(self, kind, chanid):
            if kind == "session":
                return paramiko.OPEN_SUCCEEDED
            return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

        def check_channel_direct_tcpip_request(self, chanid, origin, destination):
            if not args.forward:
                return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED
            try:
                upstream = socket.create_connection(destination, timeout=10)
            except OSError as error:
                log(f"cannot forward to {destination}: {error}")
                return paramiko.OPEN_FAILED_CONNECT_FAILED
            upstream.settimeout(None)
            self.forwards[chanid] = upstream
            return paramiko.OPEN_SUCCEEDED

        def check_channel_exec_request(self, channel, command):
            return False

    def serve(client: socket.socket) -> None:
        transport = paramiko.Transport(client)
        for host_key in host_keys:
            transport.add_server_key(host_key)
        transport.set_subsystem_handler("sftp", paramiko.SFTPServer, sftp_server)
        server = Server()
        try:
            transport.start_server(server=server)
            while transport.is_active():
                channel = transport.accept(1)
                upstream = server.forwards.pop(channel.get_id(), None) if channel else None
                if upstream is not None:
                    threading.Thread(target=pump, args=(channel, upstream), daemon=True).start()
                    threading.Thread(target=pump, args=(upstream, channel), daemon=True).start()
        except (OSError, paramiko.SSHException) as error:
            log(f"connection ended: {error}")
        finally:
            transport.close()

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(16)
    log(f"listening on 127.0.0.1:{args.port}")
    while True:
        client, _ = listener.accept()
        threading.Thread(target=serve, args=(client,), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
