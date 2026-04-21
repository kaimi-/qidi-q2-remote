import argparse
import asyncio
import logging
import math
import os
import time
from pathlib import Path
from typing import Optional, Set

from aiohttp import web

from .fb import FbParams, FramebufferCapture, autodetect


DEFAULT_SOCKET_PATH = "/run/qd2-remote-input.sock"
DEFAULT_HTTP_HOST   = "127.0.0.1"
DEFAULT_HTTP_PORT   = 18080
DEFAULT_FPS         = 5
DEFAULT_JPEG_Q      = 70
BOUNDARY            = "qd2frame"

log = logging.getLogger("qd2_remote")


class InputHub:
    """Accepts preload connections on a Unix socket and fans out text commands."""

    def __init__(self, sock_path: str):
        self.sock_path = sock_path
        self._writers: Set[asyncio.StreamWriter] = set()
        self._lock = asyncio.Lock()
        self._server: Optional[asyncio.AbstractServer] = None

    async def start(self) -> None:
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass
        parent = os.path.dirname(self.sock_path) or "."
        os.makedirs(parent, exist_ok=True)

        self._server = await asyncio.start_unix_server(self._handle, path=self.sock_path)
        try:
            os.chmod(self.sock_path, 0o660)
        except OSError:
            pass
        log.info("input hub listening on %s", self.sock_path)

    async def stop(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
        async with self._lock:
            for w in list(self._writers):
                w.close()
            self._writers.clear()
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername") or "preload"
        log.info("preload connected: %s", peer)
        async with self._lock:
            self._writers.add(writer)
        try:
            while not reader.at_eof():
                data = await reader.read(256)
                if not data:
                    break
        except (ConnectionResetError, asyncio.CancelledError):
            pass
        finally:
            async with self._lock:
                self._writers.discard(writer)
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
            log.info("preload disconnected: %s", peer)

    @property
    def client_count(self) -> int:
        return len(self._writers)

    async def send(self, line: str) -> int:
        if not line.endswith("\n"):
            line += "\n"
        payload = line.encode("utf-8", errors="ignore")
        delivered = 0
        async with self._lock:
            dead = []
            for w in self._writers:
                try:
                    w.write(payload)
                    delivered += 1
                except Exception:
                    dead.append(w)
            for w in dead:
                self._writers.discard(w)
            for w in self._writers:
                try:
                    await w.drain()
                except Exception:
                    pass
        return delivered


class FrameBroadcaster:
    def __init__(self, fb: FramebufferCapture, fps: int, jpeg_q: int):
        self.fb = fb
        self.fps = max(1, min(30, fps))
        self.jpeg_q = jpeg_q
        self._latest: bytes = b""
        self._event = asyncio.Event()
        self._task: Optional[asyncio.Task] = None
        self._stopping = False
        self.last_frame_ts: float = 0.0
        self.frame_count: int = 0

    async def start(self) -> None:
        try:
            self.fb.open()
        except Exception as e:
            log.warning("framebuffer open failed: %s (will retry on demand)", e)
        self._task = asyncio.create_task(self._loop(), name="fb-capture")

    async def stop(self) -> None:
        self._stopping = True
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
        self.fb.close()

    async def _loop(self) -> None:
        period = 1.0 / self.fps
        while not self._stopping:
            t0 = time.monotonic()
            try:
                jpeg = await asyncio.get_running_loop().run_in_executor(
                    None, self.fb.capture_jpeg, self.jpeg_q
                )
                self._latest = jpeg
                self.last_frame_ts = time.time()
                self.frame_count += 1
                self._event.set()
                self._event.clear()
            except FileNotFoundError:
                log.error("framebuffer device missing: %s", self.fb.params.device)
            except PermissionError:
                log.error("permission denied reading %s", self.fb.params.device)
            except Exception as e:
                log.exception("capture error: %s", e)
            dt = time.monotonic() - t0
            await asyncio.sleep(max(0.0, period - dt))

    @property
    def latest(self) -> bytes:
        return self._latest

    async def wait_next(self, timeout: float = 2.0) -> bool:
        try:
            await asyncio.wait_for(self._event.wait(), timeout=timeout)
            return True
        except asyncio.TimeoutError:
            return False


def _clamp(v: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, v))


def build_app(
    broadcaster: FrameBroadcaster,
    hub: InputHub,
    fb_params: FbParams,
    web_root: Path,
) -> web.Application:
    app = web.Application(client_max_size=8 * 1024)
    app["broadcaster"] = broadcaster
    app["hub"]         = hub
    app["fb_params"]   = fb_params
    app["web_root"]    = web_root

    async def index(_req: web.Request) -> web.Response:
        path = web_root / "index.html"
        return web.Response(body=path.read_bytes(), content_type="text/html")

    async def health(_req: web.Request) -> web.Response:
        return web.json_response({
            "fb": {
                "device": fb_params.device,
                "width":  fb_params.width,
                "height": fb_params.height,
                "bpp":    fb_params.bpp,
                "pixel_format": fb_params.pixel_format,
            },
            "last_frame_ts": broadcaster.last_frame_ts,
            "frame_count":   broadcaster.frame_count,
            "preload_clients": hub.client_count,
            "socket": hub.sock_path,
        })

    async def snapshot(_req: web.Request) -> web.Response:
        data = broadcaster.latest
        if not data:
            await broadcaster.wait_next()
            data = broadcaster.latest
        if not data:
            return web.Response(status=503, text="no frame")
        return web.Response(body=data, content_type="image/jpeg")

    async def stream(_req: web.Request) -> web.StreamResponse:
        resp = web.StreamResponse(status=200, reason="OK", headers={
            "Content-Type": f"multipart/x-mixed-replace; boundary={BOUNDARY}",
            "Cache-Control": "no-store, no-cache, must-revalidate, max-age=0",
            "Pragma": "no-cache",
        })
        await resp.prepare(_req)
        try:
            while True:
                if not await broadcaster.wait_next(timeout=2.0):
                    if not broadcaster.latest:
                        continue
                frame = broadcaster.latest
                if not frame:
                    continue
                head = (
                    f"--{BOUNDARY}\r\n"
                    f"Content-Type: image/jpeg\r\n"
                    f"Content-Length: {len(frame)}\r\n\r\n"
                ).encode("ascii")
                await resp.write(head)
                await resp.write(frame)
                await resp.write(b"\r\n")
        except (ConnectionResetError, asyncio.CancelledError):
            pass
        return resp

    async def input_post(req: web.Request) -> web.Response:
        try:
            data = await req.json()
        except Exception:
            return web.json_response({"error": "invalid json"}, status=400)
        action = str(data.get("action", "")).lower()

        def coord(name: str) -> int:
            v = data.get(name)
            if v is None:
                raise ValueError(f"missing {name}")
            return int(math.floor(float(v)))

        try:
            if action == "click":
                x = _clamp(coord("x"), 0, fb_params.width  - 1)
                y = _clamp(coord("y"), 0, fb_params.height - 1)
                line = f"click {x} {y}"
            elif action == "down":
                x = _clamp(coord("x"), 0, fb_params.width  - 1)
                y = _clamp(coord("y"), 0, fb_params.height - 1)
                line = f"down {x} {y}"
            elif action == "move":
                x = _clamp(coord("x"), 0, fb_params.width  - 1)
                y = _clamp(coord("y"), 0, fb_params.height - 1)
                line = f"move {x} {y}"
            elif action == "up":
                line = "up"
            else:
                return web.json_response({"error": f"unknown action {action!r}"}, status=400)
        except (TypeError, ValueError) as e:
            return web.json_response({"error": str(e)}, status=400)

        delivered = await hub.send(line)
        return web.json_response({"ok": True, "delivered": delivered, "line": line})

    app.router.add_get("/",              index)
    app.router.add_get("/health",        health)
    app.router.add_get("/stream",        stream)
    app.router.add_get("/screenshot.jpg", snapshot)
    app.router.add_post("/input",        input_post)
    return app


async def run_service(
    host: str,
    port: int,
    sock: str,
    fps: int,
    jpeg_q: int,
    fb_device: str,
    web_root: Path,
) -> None:
    params = FbParams(device=fb_device)
    params = autodetect(params)

    fb = FramebufferCapture(params)
    params.pixel_format = fb.try_formats()
    log.info("framebuffer: %dx%d bpp=%d pf=%s", params.width, params.height, params.bpp, params.pixel_format)

    broadcaster = FrameBroadcaster(fb, fps=fps, jpeg_q=jpeg_q)
    hub         = InputHub(sock)

    await hub.start()
    await broadcaster.start()

    app    = build_app(broadcaster, hub, params, web_root)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, host=host, port=port)
    await site.start()
    log.info("http serving on http://%s:%d", host, port)

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    import signal
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except NotImplementedError:
            pass

    try:
        await stop.wait()
    finally:
        log.info("shutting down")
        await runner.cleanup()
        await broadcaster.stop()
        await hub.stop()


def main() -> None:
    parser = argparse.ArgumentParser(prog="qd2-remote-service")
    parser.add_argument("--host",   default=os.environ.get("QD_HTTP_HOST", DEFAULT_HTTP_HOST))
    parser.add_argument("--port",   type=int, default=int(os.environ.get("QD_HTTP_PORT", DEFAULT_HTTP_PORT)))
    parser.add_argument("--sock",   default=os.environ.get("QD_REMOTE_INPUT_SOCK", DEFAULT_SOCKET_PATH))
    parser.add_argument("--fps",    type=int, default=DEFAULT_FPS)
    parser.add_argument("--jpeg-quality", type=int, default=DEFAULT_JPEG_Q)
    parser.add_argument("--fb",     default="/dev/fb0")
    parser.add_argument("--web-root", default=str(Path(__file__).parent / "web"))
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    asyncio.run(run_service(
        host=args.host,
        port=args.port,
        sock=args.sock,
        fps=args.fps,
        jpeg_q=args.jpeg_quality,
        fb_device=args.fb,
        web_root=Path(args.web_root),
    ))


if __name__ == "__main__":
    main()
