"""Writes Interface/FastTravelArrivalPlanner.swf: an empty Flash movie for the arrival panel.

The movie only provides a stage, the same size as the game's menus (1280 x 720), for the
plugin to build the panel on at runtime, with the game's own fonts and cursor. It holds no
ActionScript and no graphics. Run:  python tools/make_swf.py
"""
import pathlib
import struct

WIDTH, HEIGHT = 1280, 720   # pixels; the game scales menus to the screen keeping proportions
FRAME_RATE = 30.0
SWF_VERSION = 8             # ActionScript 2, as the game's own menus


def rect(xmin, xmax, ymin, ymax):
    """A SWF RECT in twips (1/20 pixel), bit-packed."""
    values = [v * 20 for v in (xmin, xmax, ymin, ymax)]
    nbits = max(max(abs(v) for v in values).bit_length() + 1, 1)
    bits = format(nbits, "05b") + "".join(format(v & ((1 << nbits) - 1), f"0{nbits}b") for v in values)
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def tag(code, body=b""):
    assert len(body) < 63
    return struct.pack("<H", (code << 6) | len(body)) + body


body = rect(0, WIDTH, 0, HEIGHT)
body += struct.pack("<H", int(FRAME_RATE * 256))    # 8.8 fixed point
body += struct.pack("<H", 1)                        # one frame
body += tag(69, struct.pack("<I", 0))               # FileAttributes: ActionScript 2, no network
body += tag(9, bytes((0, 0, 0)))                    # SetBackgroundColor (the menu shows it transparent)
body += tag(1)                                      # ShowFrame
body += tag(0)                                      # End

data = b"FWS" + bytes((SWF_VERSION,)) + struct.pack("<I", 8 + len(body)) + body
out = pathlib.Path(__file__).resolve().parent.parent / "dist" / "Interface" / "FastTravelArrivalPlanner.swf"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(data)
print(f"Wrote {out} ({len(data)} bytes)")
