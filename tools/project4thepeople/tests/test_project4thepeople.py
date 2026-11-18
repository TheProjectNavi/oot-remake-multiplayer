"""Tests for tools/project4thepeople.

Each test program in tests/programs is built as a normal Linux program, run,
then turned into C by the recompiler, built again and run. Both runs must
print the same text and end with the same exit code.

Needs Linux x86-64, gcc (clang too if installed) and `pip install capstone`.
Run: python tools/project4thepeople/tests/test_project4thepeople.py -v
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
        cls.tmp = tempfile.mkdtemp(prefix="project4thepeople_test_")

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
        r = run([sys.executable, os.path.join(TOOL, "project4thepeople.py"), "recomp", exe, "-o", out])
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        rexe = exe + ".recomp"
        r = run([sys.executable, os.path.join(TOOL, "project4thepeople.py"), "build", out, "-o", rexe])
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


SYSDEMO = os.path.join(HERE, "sysdemo")
PAD_SCRIPT = "8\n8\n17\n2\n0\n4\n24\n1\n"


@unittest.skipUnless(CAN_RUN and HAVE_CAPSTONE, "needs Linux x86-64, a C compiler and capstone")
class SystemLibraryTest(unittest.TestCase):
    """A program that uses a made-up console library (tests/sysdemo): the
    original runs with the library, the recompiled one with the PC versions
    in runtime/system. Both must behave the same."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="project4thepeople_sys_")
        with open(os.path.join(cls.tmp, "p4tp_pad.txt"), "w") as f:
            f.write(PAD_SCRIPT)
        with open(os.path.join(cls.tmp, "p4tp_data.txt"), "w") as f:
            f.write("hello from a file\n")
        r = run(["gcc", "-O2", "-shared", "-fPIC", "-nostdlib", "-fno-builtin",
                 "-fno-tree-loop-distribute-patterns", "-fno-stack-protector",
                 os.path.join(SYSDEMO, "p4tp_sys.c"), "-o", os.path.join(cls.tmp, "libp4tp_sys.so")])
        assert r.returncode == 0, r.stderr.decode()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def build(self, prog, cc, opt, extra=()):
        tag = f"{prog}-{cc}{opt}{''.join(extra)}"
        exe = os.path.join(self.tmp, tag)
        flags = [f for f in FLAGS if f != "-static"] + ["-no-pie"]
        r = run([cc, opt, *flags, *extra, os.path.join(SYSDEMO, prog + ".c"),
                 "-L", self.tmp, "-lp4tp_sys", f"-Wl,-rpath,{self.tmp}", "-lgcc", "-o", exe])
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        out = exe + ".c"
        r = run([sys.executable, os.path.join(TOOL, "project4thepeople.py"), "recomp", exe, "-o", out])
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        rexe = exe + ".recomp"
        r = run([sys.executable, os.path.join(TOOL, "project4thepeople.py"), "build", out, "-o", rexe])
        self.assertEqual(r.returncode, 0, r.stderr.decode() + r.stdout.decode())
        return exe, rexe

    def test_game_matches(self):
        variants = [(), ("-s",), ("-fno-plt",)]
        for cc in COMPILERS:
            for opt in ("-O0", "-O2", "-Os"):
                for extra in variants:
                    with self.subTest(cc=cc, opt=opt, extra=extra):
                        exe, rexe = self.build("game", cc, opt, extra)
                        native = run([exe], cwd=self.tmp)
                        recomp = run([rexe], cwd=self.tmp)
                        self.assertIn(b"frame 7 buttons 1 at 2,-1", native.stdout)
                        self.assertIn(b"called through a pointer", native.stdout)
                        self.assertEqual(recomp.stderr, b"")
                        self.assertEqual(native.stdout, recomp.stdout)
                        self.assertEqual(native.returncode, recomp.returncode)

    def test_missing_function_is_reported(self):
        exe, rexe = self.build("extra", COMPILERS[0], "-O2")
        self.assertEqual(run([exe], cwd=self.tmp).stdout, b"before\nextra 42\n")
        r = run([rexe], cwd=self.tmp)
        self.assertEqual(r.stdout, b"before\nextra not there\n")
        self.assertEqual(r.stderr, b"project4thepeople: missing system function: sys_extra\n")
        self.assertEqual(r.returncode, 3)
        r = run([rexe], cwd=self.tmp, env={**os.environ, "P4TP_STRICT": "1"})
        self.assertEqual(r.stdout, b"before\n")
        self.assertEqual(r.returncode, 126)

    def test_info_lists_imports(self):
        exe, _ = self.build("extra", COMPILERS[0], "-O2")
        r = run([sys.executable, os.path.join(TOOL, "project4thepeople.py"), "info", exe])
        text = r.stdout.decode()
        self.assertIn("libp4tp_sys.so", text)
        self.assertRegex(text, r"sys_print\s+function\s+PC version")
        self.assertRegex(text, r"sys_extra\s+function\s+no PC version yet")


if __name__ == "__main__":
    unittest.main()
