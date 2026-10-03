#!/usr/bin/env python3
"""test_import.py BUILD_DIR -- the APK/OBB import against crafted archives.

Cases: a normal install (deflated APK, stored OBB with a zip64 end record
and a zip64 extra field in the central directory, a patch OBB that wins),
a 32-bit APK, a missing OBB, a path-escaping entry, a damaged entry, too
many archives to count but none usable, an import already running (the
folder's lock held), and the lock taken on a descriptor the caller passes
(GTASA_LOCK_FD, as rf35h-gtasa does). Each case runs the native driver
(test_import) on a fresh folder and checks the files, import-status.txt and
the exit status.
"""
import errno
import fcntl
import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import zipfile

BUILD = sys.argv[1]
DRIVER = os.path.join(BUILD, "test_import")
WORK = os.path.join(BUILD, "import-cases")
rng = random.Random(1234)
failures = 0


def blob(n):
    return bytes(rng.getrandbits(8) for _ in range(n))


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("  FAIL", what)


def fresh(name):
    d = os.path.join(WORK, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def write_zip(path, entries, method=zipfile.ZIP_DEFLATED, force_zip64=False):
    with zipfile.ZipFile(path, "w", method, allowZip64=True) as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            info.compress_type = method
            with z.open(info, "w", force_zip64=force_zip64) as f:
                f.write(data)


def zip64_central_extra(path, member):
    """Rewrite one central-directory entry the way a >4 GB archive writer
    would: sizes saturated to 0xFFFFFFFF, real values in a 0x0001 extra."""
    data = bytearray(open(path, "rb").read())
    eocd = data.rfind(b"PK\x05\x06")
    cd_size, cd_off = struct.unpack_from("<II", data, eocd + 12)
    loc = eocd - 20 if data[eocd - 20:eocd - 16] == b"PK\x06\x07" else None
    if loc is not None:  # zip64 end record: the real directory values live there
        eocd64 = struct.unpack_from("<Q", data, loc + 8)[0]
        cd_size, cd_off = struct.unpack_from("<QQ", data, eocd64 + 40)
    pos = cd_off
    while pos < cd_off + cd_size:
        nlen, xlen, clen = struct.unpack_from("<HHH", data, pos + 28)
        name = data[pos + 46:pos + 46 + nlen].decode()
        if name == member:
            comp, size = struct.unpack_from("<II", data, pos + 20)
            extra = struct.pack("<HHQQ", 1, 16, size, comp)
            grow = len(extra)
            struct.pack_into("<II", data, pos + 20, 0xFFFFFFFF, 0xFFFFFFFF)
            struct.pack_into("<H", data, pos + 30, xlen + grow)
            insert = pos + 46 + nlen + xlen
            data[insert:insert] = extra
            # everything after the directory moved by `grow`: fix the sizes
            # and the locator's pointer
            if loc is not None:
                struct.pack_into("<Q", data, eocd64 + grow + 40, cd_size + grow)
                struct.pack_into("<Q", data, loc + grow + 8, eocd64 + grow)
            old32 = struct.unpack_from("<I", data, eocd + grow + 12)[0]
            if old32 != 0xFFFFFFFF:
                struct.pack_into("<I", data, eocd + grow + 12, old32 + grow)
            break
        pos += 46 + nlen + xlen + clen
    else:
        raise SystemExit("member not found: " + member)
    open(path, "wb").write(data)
    with zipfile.ZipFile(path) as z:  # still a valid archive for Python too
        assert z.testzip() is None


def run(d, **kw):
    p = subprocess.run([DRIVER, d], capture_output=True, text=True, **kw)
    try:
        status = open(os.path.join(d, "import-status.txt")).read().strip()
    except OSError:
        status = ""
    return p.returncode, status


LIB = blob(150_000)
CXX = blob(40_000)
TEX_MAIN = blob(300_000)
TEX_PATCH = blob(300_000)
IMG = blob(500_000)
SCACHE = b"shader list: everything\n" * 200

APK = [
    ("AndroidManifest.xml", blob(2000)),
    ("classes.dex", blob(30_000)),
    ("lib/arm64-v8a/libGame.so", LIB),
    ("lib/arm64-v8a/libc++_shared.so", CXX),
    ("lib/arm64-v8a/libOther.so", blob(1000)),
    ("assets/es2/shader.txt", b"void main(){}\n" * 50),
    ("assets/data/tiny.dat", b"x"),
]
# enough entries for a zip64 end record (count above 0xFFFF)
OBB = [("data/", b""), ("texdb/gta3/gta3.etc.dat", TEX_MAIN), ("models/gta3.img", IMG),
       ("scache.txt", SCACHE), ("scache_small.txt", b"short\n"), ("scache_small_low.txt", b"shorter\n")]
OBB += [("audio/sfx/%05d.raw" % i, b"%d" % i) for i in range(66_000)]

# --- 1: a complete install -------------------------------------------------------------
print("-- normal install (deflated APK, zip64 OBB, patch OBB)")
d = fresh("normal")
os.makedirs(os.path.join(d, "import"))
write_zip(os.path.join(d, "import", "base.apk"), APK)
obb = os.path.join(d, "main.8.com.rockstargames.gtasa.obb")
write_zip(obb, OBB, zipfile.ZIP_STORED)
check(b"PK\x06\x07" in open(obb, "rb").read()[-200:], "fixture has a zip64 end record")
zip64_central_extra(obb, "models/gta3.img")
write_zip(os.path.join(d, "patch.8.com.rockstargames.gtasa.obb"),
          [("texdb/gta3/gta3.etc.dat", TEX_PATCH)], zipfile.ZIP_STORED, force_zip64=True)
pending = subprocess.run([DRIVER, d, "pending"], capture_output=True, text=True).stdout.strip()
check(pending == "3", "3 archives pending, counted %s" % pending)
rc, status = run(d)
check(rc == 0, "exit status %d (%s)" % (rc, status))
check(status.startswith("done 100 "), "status line: " + status)
check(md5(os.path.join(d, "libGame.so")) == hashlib.md5(LIB).hexdigest(), "libGame.so content")
check(md5(os.path.join(d, "libc++_shared.so")) == hashlib.md5(CXX).hexdigest(), "libc++_shared.so content")
check(not os.path.exists(os.path.join(d, "libOther.so")), "only the two libraries are taken")
check(os.path.exists(os.path.join(d, "es2", "shader.txt")), "assets/ lands at the top level")
check(not os.path.exists(os.path.join(d, "AndroidManifest.xml")), "APK metadata stays out")
check(md5(os.path.join(d, "models", "gta3.img")) == hashlib.md5(IMG).hexdigest(),
      "entry with a zip64 extra field in the central directory")
check(md5(os.path.join(d, "texdb", "gta3", "gta3.etc.dat")) == hashlib.md5(TEX_PATCH).hexdigest(),
      "the patch OBB wins over the main one")
check(os.path.exists(os.path.join(d, "audio", "sfx", "65999.raw")), "last of 66000 entries")
check(open(os.path.join(d, "scache_small.txt"), "rb").read() == SCACHE, "short shader list replaced")
check(open(os.path.join(d, "scache_small.txt.orig"), "rb").read() == b"short\n", "original kept")
check(open(os.path.join(d, "scache_small_low.txt"), "rb").read() == SCACHE, "low list replaced")
moved = sorted(os.listdir(os.path.join(d, "import", "originali")))
check(moved == ["base.apk", "main.8.com.rockstargames.gtasa.obb", "patch.8.com.rockstargames.gtasa.obb"],
      "archives moved aside: %s" % moved)
parts = [f for r, _, fs in os.walk(d) for f in fs if f.endswith(".part")]
check(not parts, "no .part files left: %s" % parts[:3])
rc2, status2 = run(d)
check(rc2 == 2 and "manca l'APK" in status2, "second run finds nothing to import: " + status2)

# --- 2: a 32-bit APK --------------------------------------------------------------------
print("-- 32-bit APK")
d = fresh("apk32")
write_zip(os.path.join(d, "gtasa.apk"), [("lib/armeabi-v7a/libGame.so", LIB)])
write_zip(os.path.join(d, "main.8.com.rockstargames.gtasa.obb"), OBB[:4], zipfile.ZIP_STORED)
rc, status = run(d)
check(rc == 2 and "32 bit" in status, "32-bit APK refused: " + status)
check("armeabi-v7a" in open(os.path.join(d, "last-error.txt")).read(), "reason in last-error.txt")
check(os.path.exists(os.path.join(d, "gtasa.apk")), "nothing moved after a failure")

# --- 3: no OBB ----------------------------------------------------------------------------
print("-- missing OBB")
d = fresh("noobb")
write_zip(os.path.join(d, "gtasa.apk"), APK)
rc, status = run(d)
check(rc == 2 and "OBB" in status, "missing OBB reported: " + status)
check(not os.path.exists(os.path.join(d, "libGame.so")), "nothing extracted without an OBB")

# --- 4: an entry escaping the folder ------------------------------------------------------
print("-- path escaping the folder")
d = fresh("slip")
write_zip(os.path.join(d, "gtasa.apk"), APK)
write_zip(os.path.join(d, "main.1.obb"), [("data/ok.dat", b"ok"), ("../escaped.txt", b"bad")],
          zipfile.ZIP_STORED)
rc, status = run(d)
check(rc == 2 and "non valido" in status, "unsafe name refused: " + status)
check(not os.path.exists(os.path.join(WORK, "escaped.txt")), "nothing written outside the folder")

# --- 5: a damaged entry -----------------------------------------------------------------
print("-- damaged entry")
d = fresh("crc")
write_zip(os.path.join(d, "gtasa.apk"), APK)
obb = os.path.join(d, "main.1.obb")
write_zip(obb, [("models/gta3.img", IMG)], zipfile.ZIP_STORED)
raw = bytearray(open(obb, "rb").read())
raw[30 + len("models/gta3.img") + 1000] ^= 0xFF  # inside the stored data
open(obb, "wb").write(raw)
rc, status = run(d)
check(rc == 2 and "CRC" in status, "CRC mismatch reported: " + status)
check(not os.path.exists(os.path.join(d, "models", "gta3.img")), "damaged file not installed")
check(not os.path.exists(os.path.join(d, "models", "gta3.img.part")), "partial file removed")

# --- 6: not a ZIP at all ----------------------------------------------------------------
print("-- not a ZIP")
d = fresh("notzip")
open(os.path.join(d, "gtasa.apk"), "wb").write(blob(5000))
rc, status = run(d)
check(rc == 2 and "non si legge" in status, "garbage refused: " + status)

# --- 7: another import running --------------------------------------------------------
print("-- an import already running in the folder")
SMALL_OBB = [("data/", b""), ("texdb/gta3/gta3.etc.dat", TEX_MAIN), ("scache.txt", SCACHE)]
d = fresh("locked")
write_zip(os.path.join(d, "gtasa.apk"), APK)
write_zip(os.path.join(d, "main.8.com.rockstargames.gtasa.obb"), SMALL_OBB, zipfile.ZIP_STORED)
held = os.open(os.path.join(d, ".import.lock"), os.O_RDWR | os.O_CREAT, 0o644)
fcntl.flock(held, fcntl.LOCK_EX)
rc, status = run(d)
check(rc == 3, "a second import exits with 3, got %d" % rc)
check(status == "", "the second import left the status line alone: " + status)
check(not os.path.exists(os.path.join(d, "last-error.txt")), "no last-error.txt from the second import")
check(not os.path.exists(os.path.join(d, "libGame.so")), "the second import extracted nothing")
os.close(held)
rc, status = run(d)
check(rc == 0 and status.startswith("done 100 "), "once the lock is free the import runs: %d %s" % (rc, status))


def lock_is_free(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        return True
    except OSError as e:
        if e.errno != errno.EWOULDBLOCK:
            raise
        return False
    finally:
        os.close(fd)


# --- 8: the lock on the caller's descriptor outlives the importer --------------------------
print("-- lock on the descriptor rf35h-gtasa passes (GTASA_LOCK_FD)")
d = fresh("lockfd")
write_zip(os.path.join(d, "gtasa.apk"), APK)
write_zip(os.path.join(d, "main.8.com.rockstargames.gtasa.obb"), SMALL_OBB, zipfile.ZIP_STORED)
lock_path = os.path.join(d, ".import.lock")
ours = os.open(lock_path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
env = dict(os.environ, GTASA_LOCK_FD=str(ours))
rc, status = run(d, env=env, pass_fds=(ours,))
check(rc == 0 and status.startswith("done 100 "), "import with GTASA_LOCK_FD: %d %s" % (rc, status))
check(not lock_is_free(lock_path), "the lock ended with the importer, not with the caller's descriptor")
os.close(ours)
check(lock_is_free(lock_path), "the lock outlived the caller's descriptor")

print("import: %s" % ("ok" if not failures else "FAILED (%d)" % failures))
sys.exit(1 if failures else 0)
