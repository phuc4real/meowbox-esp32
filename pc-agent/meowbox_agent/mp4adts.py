"""Streaming MP4 (m4a) -> ADTS AAC, so the YouTube relay needs no ffmpeg: YouTube serves AAC audio in MP4 (itag 140),
the speaker decodes ADTS AAC. Only the container changes; the audio is not re-encoded.

Handles fragmented MP4 (ftyp, moov, [sidx], moof+mdat, ...: what YouTube's DASH audio looks like) and plain MP4 with
moov before mdat. Feed bytes in order with feed(); it returns complete ADTS frames as they become available.
"""
import struct

SAMPLE_RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350]


class Unsupported(Exception):
    pass


def _boxes(buf: bytes, start: int = 0, end: int | None = None):
    """(type, payload_start, box_end) for each box in buf[start:end]."""
    end = len(buf) if end is None else end
    i = start
    while i + 8 <= end:
        size, typ = struct.unpack_from(">I4s", buf, i)
        hdr = 8
        if size == 1:
            size = struct.unpack_from(">Q", buf, i + 8)[0]
            hdr = 16
        elif size == 0:
            size = end - i
        if size < hdr or i + size > end:
            return
        yield typ.decode("latin-1"), i + hdr, i + size
        i += size


def _find(buf: bytes, path: list[str], start: int = 0, end: int | None = None):
    """All boxes matching a path like ['trak', 'mdia', 'hdlr'] under buf[start:end]."""
    for typ, s, e in _boxes(buf, start, end):
        if typ == path[0]:
            if len(path) == 1:
                yield s, e
            else:
                yield from _find(buf, path[1:], s, e)


def _descriptor(buf: bytes, i: int) -> tuple[int, int, int]:
    """MPEG-4 descriptor at i -> (tag, payload start, payload end)."""
    tag = buf[i]
    i += 1
    size = 0
    for _ in range(4):
        b = buf[i]
        i += 1
        size = (size << 7) | (b & 0x7F)
        if not b & 0x80:
            break
    return tag, i, i + size


class Mp4ToAdts:
    def __init__(self):
        self.buf = bytearray()
        self.pos = 0                 # file offset of buf[0]
        self.asc = None              # (object type, sample rate index, channel config)
        self.track_id = None
        self.default_size = 0        # trex default sample size (fragmented)
        self.pending: list[int] = []  # sample sizes still to cut out of the current mdat (fragmented)
        self.plain: list[tuple[int, int]] = []  # (absolute offset, size) of samples (plain MP4)
        self.in_mdat = 0             # bytes of mdat payload still to read
        self.fragmented = False

    # -- ADTS
    def _adts(self, payload: bytes) -> bytes:
        ot, sri, ch = self.asc
        profile = max(0, min(ot, 4) - 1)  # 2-bit profile = object type - 1 (LC = 1); HE-AAC carries an LC base
        n = len(payload) + 7
        hdr = bytes([
            0xFF, 0xF1,
            (profile << 6) | (sri << 2) | ((ch >> 2) & 1),
            ((ch & 3) << 6) | ((n >> 11) & 3),
            (n >> 3) & 0xFF,
            ((n & 7) << 5) | 0x1F,
            0xFC,
        ])
        return hdr + payload

    # -- moov
    def _parse_moov(self, b: bytes, hdr: int) -> None:
        for trak_s, trak_e in _find(b, ["trak"], hdr):
            hd = next(_find(b, ["mdia", "hdlr"], trak_s, trak_e), None)
            if not hd or b[hd[0] + 8:hd[0] + 12] != b"soun":
                continue
            tk = next(_find(b, ["tkhd"], trak_s, trak_e))
            ver = b[tk[0]]
            self.track_id = struct.unpack_from(">I", b, tk[0] + (20 if ver == 1 else 12))[0]
            stbl = next(_find(b, ["mdia", "minf", "stbl"], trak_s, trak_e))
            stsd = next(_find(b, ["stsd"], *stbl))
            entry = stsd[0] + 8                                   # version/flags + entry count
            mp4a_size, mp4a_type = struct.unpack_from(">I4s", b, entry)
            if mp4a_type != b"mp4a":
                raise Unsupported(f"audio codec {mp4a_type!r}, not AAC")
            esds = next(_find(b, ["esds"], entry + 8 + 28, entry + mp4a_size))
            tag, s, e = _descriptor(b, esds[0] + 4)                # ES_Descriptor
            i = s + 3
            flags = b[s + 2]
            if flags & 0x80:
                i += 2
            if flags & 0x40:
                i += 1 + b[i]
            if flags & 0x20:
                i += 2
            tag, s, e = _descriptor(b, i)                          # DecoderConfigDescriptor
            tag, s, e = _descriptor(b, s + 13)                     # DecoderSpecificInfo = AudioSpecificConfig
            asc = int.from_bytes(b[s:s + 2], "big")
            ot, sri, ch = asc >> 11, (asc >> 7) & 0xF, (asc >> 3) & 0xF
            if sri == 15:
                raise Unsupported("explicit sample rate")
            self.asc = (ot, sri, ch)
            # plain MP4: sample table
            stsz = next(_find(b, ["stsz"], *stbl), None)
            stco = next(_find(b, ["stco"], *stbl), None) or next(_find(b, ["co64"], *stbl), None)
            stsc = next(_find(b, ["stsc"], *stbl), None)
            if stsz and stco and stsc:
                fixed, count = struct.unpack_from(">II", b, stsz[0] + 4)
                if count:
                    sizes = [fixed] * count if fixed else list(struct.unpack_from(f">{count}I", b, stsz[0] + 12))
                    big = b[stco[0] - 4:stco[0]] == b"co64"
                    n = struct.unpack_from(">I", b, stco[0] + 4)[0]
                    offs = struct.unpack_from(f">{n}{'Q' if big else 'I'}", b, stco[0] + 8)
                    m = struct.unpack_from(">I", b, stsc[0] + 4)[0]
                    runs = [struct.unpack_from(">III", b, stsc[0] + 8 + 12 * k) for k in range(m)]
                    k = 0
                    for ci, off in enumerate(offs, 1):
                        while k + 1 < len(runs) and runs[k + 1][0] <= ci:
                            k += 1
                        for _ in range(runs[k][1]):
                            if not sizes:
                                break
                            sz = sizes.pop(0)
                            self.plain.append((off, sz))
                            off += sz
                    self.plain.sort()
            break
        if not self.asc:
            raise Unsupported("no AAC audio track")
        for s, _e in _find(b, ["mvex", "trex"], hdr):
            if struct.unpack_from(">I", b, s + 4)[0] == self.track_id:
                self.default_size = struct.unpack_from(">I", b, s + 16)[0]

    def _parse_moof(self, b: bytes, hdr: int) -> None:
        self.fragmented = True
        self.pending = []
        for traf_s, traf_e in _find(b, ["traf"], hdr):
            th = next(_find(b, ["tfhd"], traf_s, traf_e))
            fl = int.from_bytes(b[th[0] + 1:th[0] + 4], "big")
            tid = struct.unpack_from(">I", b, th[0] + 4)[0]
            if self.track_id and tid != self.track_id:
                continue
            i = th[0] + 8
            i += 8 if fl & 0x1 else 0
            i += 4 if fl & 0x2 else 0
            i += 4 if fl & 0x8 else 0
            default = struct.unpack_from(">I", b, i)[0] if fl & 0x10 else self.default_size
            for ts, _te in _find(b, ["trun"], traf_s, traf_e):
                tf = int.from_bytes(b[ts + 1:ts + 4], "big")
                n = struct.unpack_from(">I", b, ts + 4)[0]
                j = ts + 8 + (4 if tf & 0x1 else 0) + (4 if tf & 0x4 else 0)
                for _ in range(n):
                    j += 4 if tf & 0x100 else 0
                    if tf & 0x200:
                        self.pending.append(struct.unpack_from(">I", b, j)[0])
                        j += 4
                    else:
                        self.pending.append(default)
                    j += 4 if tf & 0x400 else 0
                    j += 4 if tf & 0x800 else 0

    # -- streaming
    def _take(self, n: int) -> bytes:
        out = bytes(self.buf[:n])
        del self.buf[:n]
        self.pos += n
        return out

    def feed(self, data: bytes) -> list[bytes]:
        self.buf += data
        out = []
        while True:
            if self.in_mdat:
                if self.fragmented or not self.plain:
                    while self.pending and len(self.buf) >= self.pending[0] and self.in_mdat >= self.pending[0]:
                        n = self.pending.pop(0)
                        out.append(self._adts(self._take(n)))
                        self.in_mdat -= n
                    if not self.pending and self.in_mdat and len(self.buf) >= self.in_mdat:
                        self._take(self.in_mdat)  # padding / other tracks
                        self.in_mdat = 0
                    if self.in_mdat:
                        return out
                else:  # plain MP4: samples by absolute offset
                    while self.plain and self.plain[0][0] < self.pos:
                        self.plain.pop(0)
                    while self.plain:
                        off, n = self.plain[0]
                        skip = off - self.pos
                        if skip > 0:
                            k = min(skip, len(self.buf), self.in_mdat)
                            self._take(k)
                            self.in_mdat -= k
                            if self.pos < off:
                                return out
                        if len(self.buf) < n:
                            return out
                        out.append(self._adts(self._take(n)))
                        self.in_mdat -= n
                        self.plain.pop(0)
                    k = min(len(self.buf), self.in_mdat)
                    self._take(k)
                    self.in_mdat -= k
                    if self.in_mdat:
                        return out
                continue
            if len(self.buf) < 8:
                return out
            size, typ = struct.unpack_from(">I4s", self.buf, 0)
            hdr = 8
            if size == 1:
                if len(self.buf) < 16:
                    return out
                size = struct.unpack_from(">Q", self.buf, 8)[0]
                hdr = 16
            typ = typ.decode("latin-1")
            if typ == "mdat":
                if not self.asc:
                    raise Unsupported("mdat before moov (not streamable)")
                self._take(hdr)
                self.in_mdat = (size - hdr) if size else 1 << 62
                continue
            if size < hdr:
                raise Unsupported(f"bad box {typ}")
            if len(self.buf) < size:
                if size > 64 * 1024 * 1024:
                    raise Unsupported(f"box {typ} too large")
                return out
            box = self._take(size)
            if typ == "moov":
                self._parse_moov(box, hdr)
            elif typ == "moof":
                self._parse_moof(box, hdr)
