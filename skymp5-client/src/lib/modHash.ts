// The mod lock's hash (thuum docs/verbs/mod-lock.md; the one definition is
// wire-rules mods.rs): FNV-1a 64 over the plugin's name in lowercase, one
// zero byte, its size as a little-endian u32 and its CRC-32 as a
// little-endian u32, written as 16 lowercase hex digits. BigInt by calls,
// not literals: the client compiles for es5.

const FNV_OFFSET = BigInt("0xcbf29ce484222325");
const FNV_PRIME = BigInt("0x100000001b3");
const MASK64 = BigInt("0xffffffffffffffff");

function utf8Bytes(text: string): number[] {
  const out: number[] = [];
  for (let i = 0; i < text.length; i++) {
    let cp = text.charCodeAt(i);
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < text.length) {
      const low = text.charCodeAt(i + 1);
      if (low >= 0xdc00 && low <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        i++;
      }
    }
    if (cp < 0x80) {
      out.push(cp);
    } else if (cp < 0x800) {
      out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    } else {
      out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    }
  }
  return out;
}

function le32(value: number): number[] {
  const v = value >>> 0;
  return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
}

export function fnv1a64Hex(bytes: number[]): string {
  let h = FNV_OFFSET;
  for (const b of bytes) {
    h = ((h ^ BigInt(b)) * FNV_PRIME) & MASK64;
  }
  const hex = h.toString(16);
  return "0000000000000000".slice(hex.length) + hex;
}

// A plugin's hash from the three facts both ends have (Skyrim Platform's
// getFileInfo gives size and crc32; the crc may come signed, the bytes are
// the same).
export function modHash(name: string, size: number, crc32: number): string {
  const bytes = utf8Bytes(name.toLowerCase());
  bytes.push(0);
  return fnv1a64Hex(bytes.concat(le32(size), le32(crc32)));
}
