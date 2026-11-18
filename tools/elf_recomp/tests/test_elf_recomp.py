"""Tests for tools/elf_recomp.

Each test program in tests/programs is built as a normal Linux program, run,
then turned into C by the recompiler, built again and run. Both runs must
print the same text and end with the same exit code.

Needs Linux x86-64, gcc (clang too if installed) and `pip install capstone`.
Run: python tools/elf_recomp/tests/test_elf_recomp.py -v
"""

import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)

import elfparse  # noqa: E402

PROGRAMS = ["hello", "arith", "sort", "switch", "bits"]
OPT_LEVELS = ["-O0", "-O1", "-O2", "-O3", "-Os"]
COMPILERS = [c for c in ("gcc", "clang") if shutil.which(c)]
FLAGS = ["-static", "-nostdlib", "-fno-pie", "-fno-stack-protector", "-fcf-protection=none",
         "-mgeneral-regs-only", "-fno-builtin", "-fno-asynchronous-unwind-tables"]

try:
    import capstone  # noqa: F401
    HAVE_CAPSTONE = True
except ImportError:
    HAVE_CAPSTONE = False

CAN_RUN = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64") and COMPILERS


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, timeout=300, **kw)


class ElfParseTest(unittest.TestCase):
    def make_elf(self, e_type=2, ph_types=(1,)):
        """A minimal ELF64 header with program headers and no sections."""
        phnum = len(ph_types)
        hdr = bytearray(64)
        hdr[0:4] = b"\x7fELF"
        hdr[4], hdr[5], hdr[6] = 2, 1, 1
        struct.pack_into("<HHIQQQIHHHHHH", hdr, 16, e_type, 62, 1, 0x1000, 64, 0, 0,
                         64, 56, phnum, 64, 0, 0)
        for t in ph_types:
            hdr += struct.pack("<IIQQQQQQ", t, 5, 0, 0x1000, 0, 0x100, 0x100, 0x1000)
        hdr += bytes(0x100)
        return bytes(hdr)

    def test_plain_elf(self):
        elf = elfparse.parse(self.make_elf())
        self.assertEqual(elf.type_name, "ET_EXEC")
        self.assertFalse(elf.is_playstation)
        self.assertEqual(len(elf.load_segments()), 1)

    def test_playstation_types_are_named(self):
        elf = elfparse.parse(self.make_elf(0xFE10, (1, 0x61000000, 0x61000001, 0x61000010)))
        self.assertEqual(elf.type_name, "ET_SCE_DYNEXEC")
        self.assertTrue(elf.is_playstation)
        self.assertEqual([s.name for s in elf.segments],
                         ["PT_LOAD", "PT_SCE_DYNLIBDATA", "PT_SCE_PROCPARAM", "PT_SCE_RELRO"])
        self.assertEqual(len(elf.load_segments()), 2)  # PT_SCE_RELRO loads too

    def test_signed_files_are_refused(self):
        for magic in elfparse.SELF_MAGICS:
            with self.assertRaises(elfparse.ElfError) as e:
                elfparse.parse(magic + bytes(100))
            self.assertIn("does not decrypt", str(e.exception))

    def test_not_elf(self):
        with self.assertRaises(elfparse.ElfError):
            elfparse.parse(b"MZ" + bytes(100))

    def test_read_past_file_size_is_zero(self):
        data = bytearray(self.make_elf())
        elf = elfparse.parse(bytes(data))
        elf.segments[0].filesz = 4
        self.assertEqual(elf.read_vaddr(0x1002, 4), bytes(data[2:4]) + b"\0\0")


@unittest.skipUnless(CAN_RUN and HAVE_CAPSTONE, "needs Linux x86-64, a C compiler and capstone")
class RecompileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="elf_recomp_test_")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def check(self, cc, opt, prog, strip):
        tag = f"{prog}-{cc}{opt}{'-s' if strip else ''}"
        exe = os.path.join(self.tmp, tag)
        src = os.path.join(HERE, "programs", prog + ".c")
        extra = ["-s"] if strip else []
        r = run([cc, opt, *FLAGS, src, "-lgcc", *extra, "-o", exe])
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        native = run([exe])

        out = exe + ".c"
        r = run([sys.executable, os.path.join(TOOL, "elf_recomp.py"), "recomp", exe, "-o", out])
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        rexe = exe + ".recomp"
        r = run([sys.executable, os.path.join(TOOL, "elf_recomp.py"), "build", out, "-o", rexe])
        self.assertEqual(r.returncode, 0, r.stderr.decode() + r.stdout.decode())
        recomp = run([rexe])
        self.assertEqual(recomp.stderr, b"", f"{tag}: {recomp.stderr.decode()}")
        self.assertEqual(native.stdout, recomp.stdout, tag)
        self.assertEqual(native.returncode, recomp.returncode, tag)

    def test_with_symbols(self):
        for cc in COMPILERS:
            for opt in OPT_LEVELS:
                for prog in PROGRAMS:
                    with self.subTest(cc=cc, opt=opt, prog=prog):
                        self.check(cc, opt, prog, strip=False)

    def test_stripped(self):
        """No symbols, as in a shipped program: functions are found by following code."""
        for cc in COMPILERS:
            for opt in OPT_LEVELS:
                for prog in PROGRAMS:
                    with self.subTest(cc=cc, opt=opt, prog=prog):
                        self.check(cc, opt, prog, strip=True)


if __name__ == "__main__":
    unittest.main()
