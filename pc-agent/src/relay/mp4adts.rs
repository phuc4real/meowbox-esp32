//! Streaming MP4 (m4a) → ADTS AAC, so the YouTube relay needs no ffmpeg: YouTube serves AAC audio in MP4 (itag
//! 140), the speaker decodes ADTS AAC. Only the container changes; the audio is not re-encoded.
//!
//! Handles fragmented MP4 (ftyp, moov, [sidx], moof+mdat, ...: what YouTube's DASH audio looks like) and plain MP4
//! with moov before mdat. Feed bytes in order with `feed`; it returns complete ADTS frames as they become
//! available. Line-for-line port of the Python agent's mp4adts.py (the fixtures check they give the same bytes).

use std::collections::VecDeque;
use std::fmt;

#[derive(Debug, Clone, PartialEq)]
pub struct Unsupported(pub String);

impl fmt::Display for Unsupported {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Unsupported {}

type R<T> = Result<T, Unsupported>;

fn bad() -> Unsupported {
    Unsupported("malformed MP4".into())
}

fn u8_at(b: &[u8], i: usize) -> R<u8> {
    b.get(i).copied().ok_or_else(bad)
}

fn be32(b: &[u8], i: usize) -> R<u32> {
    let s = b.get(i..i + 4).ok_or_else(bad)?;
    Ok(u32::from_be_bytes([s[0], s[1], s[2], s[3]]))
}

fn be64(b: &[u8], i: usize) -> R<u64> {
    let s = b.get(i..i + 8).ok_or_else(bad)?;
    Ok(u64::from_be_bytes(s.try_into().unwrap()))
}

fn be24(b: &[u8], i: usize) -> R<u32> {
    let s = b.get(i..i + 3).ok_or_else(bad)?;
    Ok(u32::from_be_bytes([0, s[0], s[1], s[2]]))
}

/// (type, payload start, box end) for each box in buf[start..end].
fn boxes(buf: &[u8], start: usize, end: usize) -> Vec<([u8; 4], usize, usize)> {
    let mut out = Vec::new();
    let mut i = start;
    while i + 8 <= end {
        let (Ok(size32), Some(typ)) = (be32(buf, i), buf.get(i + 4..i + 8)) else { break };
        let typ: [u8; 4] = typ.try_into().unwrap();
        let (mut size, mut hdr) = (size32 as u64, 8u64);
        if size32 == 1 {
            let Ok(s) = be64(buf, i + 8) else { break };
            size = s;
            hdr = 16;
        } else if size32 == 0 {
            size = (end - i) as u64;
        }
        if size < hdr || i as u64 + size > end as u64 {
            break;
        }
        out.push((typ, i + hdr as usize, i + size as usize));
        i += size as usize;
    }
    out
}

/// All boxes matching a path like [trak, mdia, hdlr] under buf[start..end].
fn find(buf: &[u8], path: &[&[u8; 4]], start: usize, end: usize) -> Vec<(usize, usize)> {
    let mut out = Vec::new();
    for (typ, s, e) in boxes(buf, start, end) {
        if &typ == path[0] {
            if path.len() == 1 {
                out.push((s, e));
            } else {
                out.extend(find(buf, &path[1..], s, e));
            }
        }
    }
    out
}

fn first(buf: &[u8], path: &[&[u8; 4]], start: usize, end: usize) -> Option<(usize, usize)> {
    find(buf, path, start, end).into_iter().next()
}

fn need(v: Option<(usize, usize)>) -> R<(usize, usize)> {
    v.ok_or_else(bad)
}

/// MPEG-4 descriptor at i → (tag, payload start, payload end).
fn descriptor(b: &[u8], mut i: usize) -> R<(u8, usize, usize)> {
    let tag = u8_at(b, i)?;
    i += 1;
    let mut size = 0usize;
    for _ in 0..4 {
        let x = u8_at(b, i)?;
        i += 1;
        size = (size << 7) | (x & 0x7F) as usize;
        if x & 0x80 == 0 {
            break;
        }
    }
    Ok((tag, i, i + size))
}

#[derive(Default)]
pub struct Mp4ToAdts {
    buf: Vec<u8>,
    /// Read position inside `buf` (bytes before it are consumed).
    head: usize,
    /// File offset of buf[head].
    pos: u64,
    /// (object type, sample rate index, channel config)
    asc: Option<(u32, u32, u32)>,
    track_id: Option<u32>,
    /// trex default sample size (fragmented).
    default_size: u32,
    /// Sample sizes still to cut out of the current mdat (fragmented).
    pending: VecDeque<u32>,
    /// (absolute offset, size) of samples (plain MP4).
    plain: VecDeque<(u64, u32)>,
    /// Bytes of mdat payload still to read.
    in_mdat: u64,
    fragmented: bool,
}

impl Mp4ToAdts {
    pub fn new() -> Self {
        Self::default()
    }

    fn avail(&self) -> usize {
        self.buf.len() - self.head
    }

    fn take(&mut self, n: usize) -> &[u8] {
        let s = self.head;
        self.head += n;
        self.pos += n as u64;
        &self.buf[s..s + n]
    }

    fn adts(&self, payload: &[u8], out: &mut Vec<u8>) {
        let (ot, sri, ch) = self.asc.unwrap_or((2, 4, 2));
        let profile = ot.clamp(1, 4) - 1; // 2-bit profile = object type - 1 (LC = 1); HE-AAC carries an LC base
        let n = payload.len() + 7;
        out.extend_from_slice(&[
            0xFF,
            0xF1,
            ((profile << 6) | (sri << 2) | ((ch >> 2) & 1)) as u8,
            (((ch & 3) << 6) | ((n >> 11) as u32 & 3)) as u8,
            ((n >> 3) & 0xFF) as u8,
            (((n & 7) << 5) | 0x1F) as u8,
            0xFC,
        ]);
        out.extend_from_slice(payload);
    }

    fn parse_moov(&mut self, b: &[u8], hdr: usize) -> R<()> {
        for (trak_s, trak_e) in find(b, &[b"trak"], hdr, b.len()) {
            let Some(hd) = first(b, &[b"mdia", b"hdlr"], trak_s, trak_e) else { continue };
            if b.get(hd.0 + 8..hd.0 + 12) != Some(b"soun") {
                continue;
            }
            let tk = need(first(b, &[b"tkhd"], trak_s, trak_e))?;
            let ver = u8_at(b, tk.0)?;
            self.track_id = Some(be32(b, tk.0 + if ver == 1 { 20 } else { 12 })?);
            let stbl = need(first(b, &[b"mdia", b"minf", b"stbl"], trak_s, trak_e))?;
            let stsd = need(first(b, &[b"stsd"], stbl.0, stbl.1))?;
            let entry = stsd.0 + 8; // version/flags + entry count
            let mp4a_size = be32(b, entry)? as usize;
            let mp4a_type = b.get(entry + 4..entry + 8).ok_or_else(bad)?;
            if mp4a_type != b"mp4a" {
                return Err(Unsupported(format!("audio codec {:?}, not AAC", String::from_utf8_lossy(mp4a_type))));
            }
            let esds = need(first(b, &[b"esds"], entry + 8 + 28, (entry + mp4a_size).min(b.len())))?;
            let (_, s, _) = descriptor(b, esds.0 + 4)?; // ES_Descriptor
            let mut i = s + 3;
            let flags = u8_at(b, s + 2)?;
            if flags & 0x80 != 0 {
                i += 2;
            }
            if flags & 0x40 != 0 {
                i += 1 + u8_at(b, i)? as usize;
            }
            if flags & 0x20 != 0 {
                i += 2;
            }
            let (_, s, _) = descriptor(b, i)?; // DecoderConfigDescriptor
            let (_, s, _) = descriptor(b, s + 13)?; // DecoderSpecificInfo = AudioSpecificConfig
            let asc = u16::from_be_bytes([u8_at(b, s)?, u8_at(b, s + 1)?]) as u32;
            let (ot, sri, ch) = (asc >> 11, (asc >> 7) & 0xF, (asc >> 3) & 0xF);
            if sri == 15 {
                return Err(Unsupported("explicit sample rate".into()));
            }
            self.asc = Some((ot, sri, ch));
            // plain MP4: sample table
            let stsz = first(b, &[b"stsz"], stbl.0, stbl.1);
            let stco = first(b, &[b"stco"], stbl.0, stbl.1).or_else(|| first(b, &[b"co64"], stbl.0, stbl.1));
            let stsc = first(b, &[b"stsc"], stbl.0, stbl.1);
            if let (Some(stsz), Some(stco), Some(stsc)) = (stsz, stco, stsc) {
                let (fixed, count) = (be32(b, stsz.0 + 4)?, be32(b, stsz.0 + 8)? as usize);
                if count > 0 {
                    let mut sizes: VecDeque<u32> = if fixed != 0 {
                        std::iter::repeat_n(fixed, count).collect()
                    } else {
                        (0..count).map(|k| be32(b, stsz.0 + 12 + 4 * k)).collect::<R<_>>()?
                    };
                    let big = b.get(stco.0 - 4..stco.0) == Some(b"co64");
                    let n = be32(b, stco.0 + 4)? as usize;
                    let offs: Vec<u64> =
                        (0..n)
                            .map(|k| {
                                if big {
                                    be64(b, stco.0 + 8 + 8 * k)
                                } else {
                                    be32(b, stco.0 + 8 + 4 * k).map(u64::from)
                                }
                            })
                            .collect::<R<_>>()?;
                    let m = be32(b, stsc.0 + 4)? as usize;
                    let runs: Vec<(u32, u32)> = (0..m)
                        .map(|k| Ok((be32(b, stsc.0 + 8 + 12 * k)?, be32(b, stsc.0 + 12 + 12 * k)?)))
                        .collect::<R<_>>()?;
                    let mut k = 0;
                    let mut plain = Vec::new();
                    for (ci, &off) in offs.iter().enumerate() {
                        let ci = ci as u32 + 1;
                        while k + 1 < runs.len() && runs[k + 1].0 <= ci {
                            k += 1;
                        }
                        let mut off = off;
                        for _ in 0..runs.get(k).map_or(0, |r| r.1) {
                            let Some(sz) = sizes.pop_front() else { break };
                            plain.push((off, sz));
                            off += sz as u64;
                        }
                    }
                    plain.sort();
                    self.plain = plain.into();
                }
            }
            break;
        }
        if self.asc.is_none() {
            return Err(Unsupported("no AAC audio track".into()));
        }
        for (s, _) in find(b, &[b"mvex", b"trex"], hdr, b.len()) {
            if Some(be32(b, s + 4)?) == self.track_id {
                self.default_size = be32(b, s + 16)?;
            }
        }
        Ok(())
    }

    fn parse_moof(&mut self, b: &[u8], hdr: usize) -> R<()> {
        self.fragmented = true;
        self.pending.clear();
        for (traf_s, traf_e) in find(b, &[b"traf"], hdr, b.len()) {
            let th = need(first(b, &[b"tfhd"], traf_s, traf_e))?;
            let fl = be24(b, th.0 + 1)?;
            let tid = be32(b, th.0 + 4)?;
            if self.track_id.is_some_and(|t| t != 0 && t != tid) {
                continue;
            }
            let mut i = th.0 + 8;
            i += if fl & 0x1 != 0 { 8 } else { 0 };
            i += if fl & 0x2 != 0 { 4 } else { 0 };
            i += if fl & 0x8 != 0 { 4 } else { 0 };
            let default = if fl & 0x10 != 0 { be32(b, i)? } else { self.default_size };
            for (ts, _) in find(b, &[b"trun"], traf_s, traf_e) {
                let tf = be24(b, ts + 1)?;
                let n = be32(b, ts + 4)?;
                let mut j = ts + 8 + if tf & 0x1 != 0 { 4 } else { 0 } + if tf & 0x4 != 0 { 4 } else { 0 };
                for _ in 0..n {
                    j += if tf & 0x100 != 0 { 4 } else { 0 };
                    if tf & 0x200 != 0 {
                        self.pending.push_back(be32(b, j)?);
                        j += 4;
                    } else {
                        self.pending.push_back(default);
                    }
                    j += if tf & 0x400 != 0 { 4 } else { 0 };
                    j += if tf & 0x800 != 0 { 4 } else { 0 };
                }
            }
        }
        Ok(())
    }

    /// Feed the next bytes; returns the complete ADTS frames now available, joined.
    pub fn feed(&mut self, data: &[u8]) -> R<Vec<u8>> {
        if self.head > 0 && self.head >= self.buf.len() / 2 {
            self.buf.drain(..self.head);
            self.head = 0;
        }
        self.buf.extend_from_slice(data);
        let mut out = Vec::new();
        loop {
            if self.in_mdat > 0 {
                if self.fragmented || self.plain.is_empty() {
                    while let Some(&n) = self.pending.front() {
                        let n64 = n as u64;
                        if (self.avail() as u64) < n64 || self.in_mdat < n64 {
                            break;
                        }
                        self.pending.pop_front();
                        let frame = self.take(n as usize).to_vec();
                        self.adts(&frame, &mut out);
                        self.in_mdat -= n64;
                    }
                    if self.pending.is_empty() && self.in_mdat > 0 && self.avail() as u64 >= self.in_mdat {
                        let k = self.in_mdat as usize; // padding / other tracks
                        self.take(k);
                        self.in_mdat = 0;
                    }
                    if self.in_mdat > 0 {
                        return Ok(out);
                    }
                } else {
                    // plain MP4: samples by absolute offset
                    while self.plain.front().is_some_and(|&(off, _)| off < self.pos) {
                        self.plain.pop_front();
                    }
                    while let Some(&(off, n)) = self.plain.front() {
                        if off > self.pos {
                            let k = (off - self.pos).min(self.avail() as u64).min(self.in_mdat);
                            self.take(k as usize);
                            self.in_mdat -= k;
                            if self.pos < off {
                                return Ok(out);
                            }
                        }
                        if self.avail() < n as usize {
                            return Ok(out);
                        }
                        let frame = self.take(n as usize).to_vec();
                        self.adts(&frame, &mut out);
                        self.in_mdat = self.in_mdat.saturating_sub(n as u64);
                        self.plain.pop_front();
                    }
                    let k = (self.avail() as u64).min(self.in_mdat);
                    self.take(k as usize);
                    self.in_mdat -= k;
                    if self.in_mdat > 0 {
                        return Ok(out);
                    }
                }
                continue;
            }
            if self.avail() < 8 {
                return Ok(out);
            }
            let h = self.head;
            let mut size = be32(&self.buf, h)? as u64;
            let typ: [u8; 4] = self.buf[h + 4..h + 8].try_into().unwrap();
            let mut hdr = 8u64;
            if size == 1 {
                if self.avail() < 16 {
                    return Ok(out);
                }
                size = be64(&self.buf, h + 8)?;
                hdr = 16;
            }
            let name = String::from_utf8_lossy(&typ).into_owned();
            if &typ == b"mdat" {
                if self.asc.is_none() {
                    return Err(Unsupported("mdat before moov (not streamable)".into()));
                }
                self.take(hdr as usize);
                self.in_mdat = if size != 0 { size.saturating_sub(hdr) } else { 1 << 62 };
                continue;
            }
            if size < hdr {
                return Err(Unsupported(format!("bad box {name}")));
            }
            if (self.avail() as u64) < size {
                if size > 64 * 1024 * 1024 {
                    return Err(Unsupported(format!("box {name} too large")));
                }
                return Ok(out);
            }
            let bx = self.take(size as usize).to_vec();
            match &typ {
                b"moov" => self.parse_moov(&bx, hdr as usize)?,
                b"moof" => self.parse_moof(&bx, hdr as usize)?,
                _ => {}
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const CASES: &[&str] = &["plain_stereo_44k", "frag_stereo_44k", "frag_mono_48k"];

    fn fixture(name: &str) -> Vec<u8> {
        std::fs::read(format!("{}/fixtures/{name}", env!("CARGO_MANIFEST_DIR"))).unwrap()
    }

    #[test]
    fn same_bytes_as_python_whole_file() {
        for c in CASES {
            let out = Mp4ToAdts::new().feed(&fixture(&format!("{c}.m4a"))).unwrap();
            assert_eq!(out, fixture(&format!("{c}.adts")), "{c}");
        }
    }

    #[test]
    fn same_bytes_in_any_chunk_size() {
        for c in CASES {
            let mp4 = fixture(&format!("{c}.m4a"));
            let want = fixture(&format!("{c}.adts"));
            // deterministic "random" chunk sizes 1..5000
            let mut seed: u32 = 12345;
            for _ in 0..5 {
                let mut conv = Mp4ToAdts::new();
                let (mut out, mut i) = (Vec::new(), 0);
                while i < mp4.len() {
                    seed = seed.wrapping_mul(1_103_515_245).wrapping_add(12345);
                    let n = 1 + (seed >> 16) as usize % 5000;
                    let end = (i + n).min(mp4.len());
                    out.extend(conv.feed(&mp4[i..end]).unwrap());
                    i = end;
                }
                assert_eq!(out, want, "{c}");
            }
        }
    }

    #[test]
    fn adts_header_fields() {
        let out = Mp4ToAdts::new().feed(&fixture("frag_mono_48k.m4a")).unwrap();
        assert_eq!(&out[..2], &[0xFF, 0xF1]);
        assert_eq!(out[2] >> 6, 1, "AAC LC");
        assert_eq!((out[2] >> 2) & 0xF, 3, "48 kHz");
        let ch = ((out[2] & 1) << 2) | (out[3] >> 6);
        assert_eq!(ch, 1, "mono");
    }

    #[test]
    fn garbage_and_truncation_do_not_panic() {
        let mp4 = fixture("frag_stereo_44k.m4a");
        assert!(Mp4ToAdts::new().feed(b"\x00\x00\x00\x10mdat0123456789").is_err(), "mdat before moov");
        assert!(Mp4ToAdts::new().feed(b"\x00\x00\x00\x04ftyp").is_err(), "box smaller than its header");
        for cut in [10, 100, 700, mp4.len() / 2] {
            let mut m = mp4[..cut].to_vec();
            // corrupt a byte inside moov
            if cut > 200 {
                m[150] ^= 0xFF;
            }
            let _ = Mp4ToAdts::new().feed(&m);
        }
    }
}
