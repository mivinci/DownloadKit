#!/usr/bin/env python3
"""
ws_echo_server.py - Simple WebSocket echo server for testing.

Usage:
    python3 ws_echo_server.py [port]

Echoes every message back to the sender. Logs connections and
messages to stdout. Default port is 9000.

Requirements:
    pip install websockets
"""

import asyncio
import sys

import websockets


async def handler(ws):
    addr = ws.remote_address
    print(f"[server] client connected: {addr}")
    try:
        async for msg in ws:
            print(f"[server] recv ({len(msg)} bytes): {msg!r}")
            await ws.send(msg)
            print(f"[server] echo ({len(msg)} bytes)")
    except websockets.ConnectionClosed as e:
        print(f"[server] client disconnected: {addr} "
              f"(code={e.code})")


async def main(port: int):
    print(f"[server] listening on ws://0.0.0.0:{port}")
    async with websockets.serve(
        handler, "0.0.0.0", port,
        compression="deflate",  # enable permessage-deflate
    ):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    try:
        asyncio.run(main(port))
    except KeyboardInterrupt:
        print("\n[server] stopped")
