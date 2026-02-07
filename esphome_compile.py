"""Connect to ESPHome dashboard WebSocket and trigger compile + upload.

ESPHome dashboard uses Tornado WebSockets with a message-based protocol:
1. Connect to ws://<host>:<port>/<endpoint>
2. Send: {"type": "spawn", "configuration": "<file>", ...}
3. Receive: {"event": "line", "data": "..."} for output lines
4. Receive: {"event": "exit", "code": N} when done
"""
import json
import sys
import websocket

ESPHOME_HOST = "10.10.10.3"
ESPHOME_PORT = 6052
CONFIG = "klimaanlage-wohnzimmer.yaml"

def run_ws_command(endpoint, spawn_msg):
    url = f"ws://{ESPHOME_HOST}:{ESPHOME_PORT}/{endpoint}"
    print(f"Connecting to {url} ...")
    ws = websocket.create_connection(url, timeout=600)
    print("Connected. Sending spawn command...")
    
    # ESPHome expects {"type": "spawn", ...} to start the process
    spawn_msg["type"] = "spawn"
    ws.send(json.dumps(spawn_msg))
    print(f"Sent: {json.dumps(spawn_msg)}")
    print("--- Output ---", flush=True)
    
    exit_code = None
    while True:
        try:
            raw = ws.recv()
            if not raw:
                break
            data = json.loads(raw)
            event = data.get("event", "")
            if event == "line":
                line = data.get("data", "")
                print(line, end="", flush=True)
            elif event == "exit":
                exit_code = data.get("code", -1)
                print(f"\n--- Exit code: {exit_code} ---")
                break
            else:
                print(f"[{event}] {data}", flush=True)
        except websocket.WebSocketConnectionClosedException:
            print("\nConnection closed by server")
            break
        except Exception as e:
            print(f"\nError: {e}")
            break
    
    ws.close()
    return exit_code

if __name__ == "__main__":
    action = sys.argv[1] if len(sys.argv) > 1 else "compile"
    
    if action == "compile":
        msg = {"configuration": CONFIG}
        code = run_ws_command("compile", msg)
    elif action == "upload":
        msg = {"configuration": CONFIG, "port": "OTA"}
        code = run_ws_command("upload", msg)
    elif action == "run":
        # compile + upload in one step
        msg = {"configuration": CONFIG, "port": "OTA"}
        code = run_ws_command("run", msg)
    else:
        print(f"Unknown action: {action}")
        sys.exit(1)
    
    sys.exit(0 if code == 0 else 1)
