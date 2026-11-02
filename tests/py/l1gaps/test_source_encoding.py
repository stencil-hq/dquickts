# Source files with a PEP 263 coding cookie for a codec of the registry
# (cp1252, latin9, koi8-r, shift_jis, euc-jp...): import, compile() and
# exec() of the bytes, and the SyntaxErrors of undecodable sources.
import importlib
import os
import shutil
import sys
import tempfile

CASES = {
    "cp1252": b'# -*- coding: cp1252 -*-\nx = "caf\xe9 \x80"\n',
    "latin9": b'# coding: latin9\nx = "\xa4"\n',
    "koi8_r": b'# vim: set fileencoding=koi8-r :\nx = "\xd0\xd2\xc9"\n',
    "shift_jis": b'# coding: shift_jis\nx = "\x93\xfa\x96\x7b \x95\x5c"\n',
    "euc_jp": b'# coding: euc-jp\nx = "\xc6\xfc"\n',
    "line2": b'#!/usr/bin/env python\n# coding: cp1251\nx = "\xcf\xf0"\n',
    "line3": b'#!/usr/bin/env python\n\n# coding: koi8-r\nx = "\xd0\xd2"\n',
    "utf8sig": b'# coding: utf-8-sig\nx = "\xc3\xa9"\n',
    "bom_utf8": b'\xef\xbb\xbf# coding: utf-8\nx = "\xc3\xa9"\n',
    "bom_cookie": b'\xef\xbb\xbf# coding: cp1252\nx = 1\n',
    "bogus": b'# coding: bogus-enc\nx = 1\n',
    "utf16": b'# coding: utf-16\nx = 1\n',
    "undefined": b'# coding: cp1252\nx = 1\n\n\ny = "\x81"\n',
    "ascii": b'# coding: ascii\nx = 1\ny = "\xe9"\n',
    "nocookie": b'x = 1\ny = "\xe9"\n',
    "latin1": b'# coding: latin-1\nx = "\xe9"\n',
}

tmp = tempfile.mkdtemp()
sys.path.insert(0, tmp)
try:
    for name, data in CASES.items():
        mod = "l1enc_" + name
        with open(os.path.join(tmp, mod + ".py"), "wb") as f:
            f.write(data)
    importlib.invalidate_caches()
    for name, data in CASES.items():
        mod = "l1enc_" + name
        for how in ["import", "compile", "exec"]:
            try:
                if how == "import":
                    x = importlib.import_module(mod).x
                elif how == "compile":
                    ns = {}
                    exec(compile(data, mod + ".py", "exec"), ns)
                    x = ns["x"]
                else:
                    ns = {}
                    exec(data, ns)
                    x = ns["x"]
                print(name, how, ascii(x))
            except SyntaxError as e:
                print(name, how, type(e).__name__, ascii(e.msg),
                      (os.path.basename(e.filename) if e.filename else e.filename),
                      e.lineno, e.offset, ascii(e.text), e.end_lineno, e.end_offset)
finally:
    shutil.rmtree(tmp)
