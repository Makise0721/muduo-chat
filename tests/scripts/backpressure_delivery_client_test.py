#!/usr/bin/env python3
"""Focused contract tests for the public V2Client socket seam."""
import json
import socket
import struct
import unittest

from backpressure_delivery_test import V2Client


class FakeSocket(object):
    def __init__(self, *recv_results):
        self.recv_results = list(recv_results)
        self.timeouts = []

    def settimeout(self, timeout):
        self.timeouts.append(timeout)

    def recv(self, _size):
        result = self.recv_results.pop(0)
        if isinstance(result, BaseException):
            raise result
        return result


def frame(body):
    return V2Client.frame(body)


class V2ClientRecvContractTest(unittest.TestCase):
    def client(self, *recv_results):
        return V2Client("injected", 0, sock=FakeSocket(*recv_results))

    def test_timeout_is_temporary_no_frame(self):
        client = self.client(socket.timeout("would block"))
        self.assertIsNone(client.recv(0.1))
        self.assertEqual("timeout", client.last_recv_status)

    def test_eof_is_reader_error(self):
        with self.assertRaises(EOFError):
            self.client(b"").recv(0.1)

    def test_connection_reset_is_reader_error(self):
        with self.assertRaises(ConnectionResetError):
            self.client(ConnectionResetError("peer reset")).recv(0.1)

    def test_os_error_is_reader_error(self):
        with self.assertRaises(OSError):
            self.client(OSError("socket failed")).recv(0.1)

    def test_malformed_json_is_reader_error(self):
        with self.assertRaises(json.JSONDecodeError):
            self.client(frame(b"not-json")).recv(0.1)


if __name__ == "__main__":
    unittest.main()
