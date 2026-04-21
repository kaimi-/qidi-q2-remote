import io
import os
from dataclasses import dataclass
from typing import Optional

from PIL import Image


@dataclass
class FbParams:
    device: str = "/dev/fb0"
    width: int = 480
    height: int = 272
    bpp: int = 32
    pixel_format: str = "BGRA"

    @property
    def frame_bytes(self) -> int:
        return self.width * self.height * (self.bpp // 8)


SYSFS_SIZE = "/sys/class/graphics/fb0/virtual_size"
SYSFS_BPP  = "/sys/class/graphics/fb0/bits_per_pixel"


def autodetect(params: FbParams) -> FbParams:
    try:
        with open(SYSFS_SIZE) as f:
            w, h = f.read().strip().split(",")
            params.width  = int(w)
            params.height = int(h)
    except OSError:
        pass
    try:
        with open(SYSFS_BPP) as f:
            params.bpp = int(f.read().strip())
    except OSError:
        pass
    return params


class FramebufferCapture:
    _FORMATS = ("BGRA", "BGRX", "RGBA", "RGBX")

    def __init__(self, params: FbParams):
        self.params = params
        self._fd: Optional[int] = None

    def open(self) -> None:
        if self._fd is None:
            self._fd = os.open(self.params.device, os.O_RDONLY)

    def close(self) -> None:
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def read_raw(self) -> bytes:
        if self._fd is None:
            self.open()
        assert self._fd is not None
        os.lseek(self._fd, 0, os.SEEK_SET)
        need = self.params.frame_bytes
        chunks = []
        remaining = need
        while remaining > 0:
            chunk = os.read(self._fd, remaining)
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _decode(self, raw: bytes, fmt: str) -> Image.Image:
        img = Image.frombytes(
            "RGBA" if fmt.endswith("A") else "RGBX",
            (self.params.width, self.params.height),
            raw,
            "raw",
            fmt,
        )
        if fmt.endswith("X"):
            img = img.convert("RGB")
        else:
            img = img.convert("RGB")
        return img

    def capture_jpeg(self, quality: int = 70) -> bytes:
        raw = self.read_raw()
        if len(raw) < self.params.frame_bytes:
            raw = raw + b"\x00" * (self.params.frame_bytes - len(raw))
        img = self._decode(raw, self.params.pixel_format)
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=quality)
        return buf.getvalue()

    def try_formats(self) -> str:
        for fmt in self._FORMATS:
            try:
                self.params.pixel_format = fmt
                _ = self.capture_jpeg()
                return fmt
            except Exception:
                continue
        self.params.pixel_format = "BGRA"
        return "BGRA"
