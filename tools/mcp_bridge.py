#!/usr/bin/env python3
"""
MCP bridge for Sonic 1 Recompiler game runner.

Speaks MCP (Model Context Protocol) over stdio on one side,
and line-delimited JSON over TCP to the game on the other.

Usage: python mcp_bridge.py [--port PORT]

Configure in .mcp.json:
{
  "mcpServers": {
    "sonic-runner": {
      "command": "python",
      "args": ["sonicthehedgehog/tools/mcp_bridge.py"]
    }
  }
}
"""

import sys
import json
import socket
import argparse
import traceback

# -- TCP Client ---------------------------------------------------------------

class SonicConnection:
    def __init__(self, host="127.0.0.1", port=4378):
        self.host = host
        self.port = port
        self.sock = None
        self.buf = b""

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=10)

    def is_connected(self):
        return self.sock is not None

    def send_cmd(self, cmd_dict):
        """Send a command and wait for the JSON response line."""
        if not self.sock:
            raise ConnectionError("Not connected to game")
        msg = json.dumps(cmd_dict) + "\n"
        self.sock.sendall(msg.encode("utf-8"))
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("Game disconnected")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

# -- MCP Tool Definitions -----------------------------------------------------

TOOLS = [
    {
        "name": "sonic_ping",
        "description": "Check connection to running game. Returns current frame number.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "sonic_get_registers",
        "description": "Get 68K CPU register state: D0-D7, A0-A7, SR, PC, USP, flags (C,V,Z,N,X,S), interrupt mask.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "sonic_read_memory",
        "description": "Read bytes from 68K address space (ROM, RAM, VDP, etc). Returns hex string.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "addr": {"type": "string", "description": "Hex address (e.g. '0xFF0000' for work RAM)"},
                "size": {"type": "integer", "description": "Bytes to read (1-4096)", "default": 16}
            },
            "required": ["addr"]
        }
    },
    {
        "name": "sonic_write_memory",
        "description": "Write hex bytes to 68K address space.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "addr": {"type": "string", "description": "Hex address"},
                "hex": {"type": "string", "description": "Hex bytes to write (e.g. 'FF0042')"}
            },
            "required": ["addr", "hex"]
        }
    },
    {
        "name": "sonic_read_ram",
        "description": "Read directly from work RAM shadow (fast, no bus routing). Offset is within $FF0000-$FFFFFF.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "addr": {"type": "string", "description": "RAM offset (e.g. '0xD000' for Sonic object)"},
                "size": {"type": "integer", "description": "Bytes to read (1-4096)", "default": 16}
            },
            "required": ["addr"]
        }
    },
    {
        "name": "sonic_state",
        "description": "Get Sonic's current state: position (x,y), velocity (xvel,yvel), inertia, routine, status, angle, game mode, joypad.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "sonic_history",
        "description": "Get Sonic's state over a range of frames (time-series). Shows how position, velocity, routine evolve. Max 600 frames per request.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "start": {"type": "integer", "description": "Start frame number"},
                "end": {"type": "integer", "description": "End frame number (inclusive)"}
            },
            "required": ["start", "end"]
        }
    },
    {
        "name": "sonic_object_table",
        "description": "Dump active objects from the object status table ($D000-$DFFF). Shows slot, ID, position, velocity, routine for each active object.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "count": {"type": "integer", "description": "Max objects to scan (default 64)", "default": 64}
            }
        }
    },
    {
        "name": "sonic_vblank_info",
        "description": "Get VBlank timing state: cycle accumulator, threshold, interrupt mask, frame count.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "sonic_frame_info",
        "description": "Get frame history ring buffer status: current frame, oldest available, capacity (36000 = 10 min).",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "sonic_frame_range",
        "description": "Get summary of frames in a range: game mode, Sonic yvel, routine, joypad, scroll. Max 600 frames.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "start": {"type": "integer", "description": "Start frame number"},
                "end": {"type": "integer", "description": "End frame number (inclusive)"}
            },
            "required": ["start", "end"]
        }
    },
    {
        "name": "sonic_run_frames",
        "description": "Run N extra frames of emulation (turbo, no rendering delay). Max 36000.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "count": {"type": "integer", "description": "Number of frames to run (1-36000)"}
            },
            "required": ["count"]
        }
    },
    {
        "name": "sonic_set_input",
        "description": "Override joypad input. Genesis bits: Up=0,Down=1,Left=2,Right=3,B=4,C=5,A=6,Start=7.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "keys": {"type": "string", "description": "Hex bitmask of pressed keys (e.g. '0x40' for A button)"}
            },
            "required": ["keys"]
        }
    },
    {
        "name": "sonic_watch",
        "description": "Set a watchpoint on a RAM address. Notifications sent when value changes.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "addr": {"type": "string", "description": "RAM address to watch (e.g. '0xD012' for Sonic Y velocity)"}
            },
            "required": ["addr"]
        }
    },
    {
        "name": "sonic_unwatch",
        "description": "Remove a watchpoint.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "addr": {"type": "string", "description": "RAM address to unwatch"}
            },
            "required": ["addr"]
        }
    },
    {
        "name": "sonic_quit",
        "description": "Cleanly shut down the game runner.",
        "inputSchema": {"type": "object", "properties": {}}
    },
]

# -- MCP stdio transport -------------------------------------------------------

def read_mcp_message():
    """Read one MCP message from stdin (LSP framing)."""
    content_length = 0
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.strip()
        if line == b"":
            break
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":")[1].strip())
    if content_length == 0:
        return None
    body = sys.stdin.buffer.read(content_length)
    return json.loads(body.decode("utf-8"))


def write_mcp_message(msg):
    """Write one MCP message to stdout (LSP framing)."""
    body = json.dumps(msg).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("utf-8")
    sys.stdout.buffer.write(header + body)
    sys.stdout.buffer.flush()


# -- MCP Request Handling -------------------------------------------------------

_request_id = 0

# Map MCP tool name -> TCP command name and extra params
TOOL_MAP = {
    "sonic_ping":         {"cmd": "ping"},
    "sonic_get_registers":{"cmd": "get_registers"},
    "sonic_read_memory":  {"cmd": "read_memory",  "pass": ["addr", "size"]},
    "sonic_write_memory": {"cmd": "write_memory",  "pass": ["addr", "hex"]},
    "sonic_read_ram":     {"cmd": "read_ram",      "pass": ["addr", "size"]},
    "sonic_state":        {"cmd": "sonic_state"},
    "sonic_history":      {"cmd": "sonic_history", "pass": ["start", "end"]},
    "sonic_object_table": {"cmd": "object_table",  "pass": ["count"]},
    "sonic_vblank_info":  {"cmd": "vblank_info"},
    "sonic_frame_info":   {"cmd": "frame_info"},
    "sonic_frame_range":  {"cmd": "frame_range",   "pass": ["start", "end"]},
    "sonic_run_frames":   {"cmd": "run_frames",    "pass": ["count"]},
    "sonic_set_input":    {"cmd": "set_input",     "pass": ["keys"]},
    "sonic_watch":        {"cmd": "watch",         "pass": ["addr"]},
    "sonic_unwatch":      {"cmd": "unwatch",       "pass": ["addr"]},
    "sonic_quit":         {"cmd": "quit"},
}


def handle_tool_call(conn, name, arguments):
    global _request_id
    _request_id += 1

    mapping = TOOL_MAP.get(name)
    if not mapping:
        return {"isError": True, "content": [{"type": "text", "text": f"Unknown tool: {name}"}]}

    tcp_cmd = {"cmd": mapping["cmd"], "id": _request_id}
    for key in mapping.get("pass", []):
        if key in arguments:
            tcp_cmd[key] = arguments[key]

    r = conn.send_cmd(tcp_cmd)
    text = json.dumps(r, indent=2)
    return {"content": [{"type": "text", "text": text}]}


def handle_mcp_request(conn, msg):
    method = msg.get("method", "")
    req_id = msg.get("id")
    params = msg.get("params", {})

    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "sonic-runner", "version": "0.1.0"}
            }
        }
    elif method == "notifications/initialized":
        return None
    elif method == "tools/list":
        return {"jsonrpc": "2.0", "id": req_id, "result": {"tools": TOOLS}}
    elif method == "tools/call":
        tool_name = params.get("name", "")
        arguments = params.get("arguments", {})
        try:
            result = handle_tool_call(conn, tool_name, arguments)
        except Exception as e:
            result = {"isError": True, "content": [{"type": "text", "text": str(e)}]}
        return {"jsonrpc": "2.0", "id": req_id, "result": result}
    elif method == "ping":
        return {"jsonrpc": "2.0", "id": req_id, "result": {}}
    else:
        return {"jsonrpc": "2.0", "id": req_id,
                "error": {"code": -32601, "message": f"Method not found: {method}"}}


# -- Main -----------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="MCP bridge for Sonic 1 recompiler runner")
    parser.add_argument("--port", type=int, default=4378, help="TCP port of game runner")
    parser.add_argument("--host", default="127.0.0.1", help="Host of game runner")
    args = parser.parse_args()

    conn = SonicConnection(args.host, args.port)
    connected = False

    while True:
        msg = read_mcp_message()
        if msg is None:
            break

        method = msg.get("method", "")

        if method == "tools/call" and not connected:
            try:
                conn.connect()
                connected = True
            except Exception as e:
                write_mcp_message({
                    "jsonrpc": "2.0",
                    "id": msg.get("id"),
                    "result": {
                        "isError": True,
                        "content": [{"type": "text", "text":
                            f"Cannot connect to game on {args.host}:{args.port}: {e}. "
                            f"Is SonicTheHedgehogRecomp.exe running?"}]
                    }
                })
                continue

        resp = handle_mcp_request(conn, msg)
        if resp is not None:
            write_mcp_message(resp)

    conn.close()


if __name__ == "__main__":
    main()
