"""Reader for the Asteroids demo media.db pack.

Format (reversed from SQLiteFileSystem, Asteroids.exe 0x14009A720 / 0x14009AA70):
  table files(name TEXT PK, mtime INT, compressed INT, original_size INT, data BLOB)
  key  = PBKDF2-HMAC-SHA1(password, salt=name, iterations=1000, dklen=16)
  data = IV[16] || AES-128-CBC(key, IV, payload)
  payload[:compressed] is an LZ4 block when compressed > 0, else payload[:original_size] is raw.
"""
import hashlib
import sqlite3

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PASSWORD = b"HjLxk8CwekjjquQM"
PBKDF2_ITERATIONS = 1000

try:
    import lz4.block as _lz4

    def lz4_decompress(src, size):
        return _lz4.decompress(src, uncompressed_size=size)
except ImportError:
    def lz4_decompress(src, size):
        dst = bytearray(size)
        i = o = 0
        n = len(src)
        while i < n:
            token = src[i]
            i += 1
            lit = token >> 4
            if lit == 15:
                while True:
                    b = src[i]
                    i += 1
                    lit += b
                    if b != 255:
                        break
            dst[o:o + lit] = src[i:i + lit]
            i += lit
            o += lit
            if i >= n:
                break
            off = src[i] | (src[i + 1] << 8)
            i += 2
            m = token & 15
            if m == 15:
                while True:
                    b = src[i]
                    i += 1
                    m += b
                    if b != 255:
                        break
            m += 4
            s = o - off
            if off >= m:
                dst[o:o + m] = dst[s:s + m]
            else:
                # overlapping match: replicate the period
                pat = bytes(dst[s:o])
                rep = (pat * (m // off + 1))[:m]
                dst[o:o + m] = rep
            o += m
        if o != size:
            raise ValueError(f"lz4 size mismatch {o} != {size}")
        return bytes(dst)


class MediaDB:
    def __init__(self, path, password=PASSWORD):
        self.conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        self.password = password

    def names(self):
        return [r[0] for r in self.conn.execute("SELECT name FROM files ORDER BY name")]

    def entries(self):
        return self.conn.execute(
            "SELECT name, mtime, compressed, original_size FROM files ORDER BY name").fetchall()

    def read(self, name):
        row = self.conn.execute(
            "SELECT data, compressed, original_size FROM files WHERE name=?1 LIMIT 1", (name,)).fetchone()
        if row is None:
            raise KeyError(name)
        data, compressed, original_size = row
        key = hashlib.pbkdf2_hmac("sha1", self.password, name.replace("\\", "/").encode(),
                                  PBKDF2_ITERATIONS, 16)
        dec = Cipher(algorithms.AES(key), modes.CBC(data[:16])).decryptor()
        payload = dec.update(data[16:]) + dec.finalize()
        if compressed > 0:
            return lz4_decompress(payload[:compressed], original_size)
        return payload[:original_size]
