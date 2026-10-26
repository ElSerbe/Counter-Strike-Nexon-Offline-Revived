"""Loopback-only HTTP stub for the game's plain-HTTP stats/log requests (port 80).
Logs each request line and answers 200 with an empty JSON body. Research tool."""
import asyncio, sys, time

PORT = 80
LOG = sys.argv[1] if len(sys.argv) > 1 else "http_stub.log"


def log(msg):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%H:%M:%S')} {msg}\n")


async def handle(reader, writer):
    peer = writer.get_extra_info("peername")
    try:
        data = await asyncio.wait_for(reader.read(65536), timeout=5)
        first = data.split(b"\r\n", 1)[0]
        log(f"{peer} {first!r}")
        body = b"{}"
        resp = (b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                b"Content-Length: " + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
        writer.write(resp)
        await writer.drain()
    except Exception as e:  # noqa: BLE001
        log(f"{peer} error {e!r}")
    finally:
        writer.close()


async def main():
    srv = await asyncio.start_server(handle, "127.0.0.1", PORT)
    log(f"http stub listening on 127.0.0.1:{PORT}")
    async with srv:
        await srv.serve_forever()


asyncio.run(main())
