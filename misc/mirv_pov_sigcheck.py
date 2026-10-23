#!/usr/bin/env python
"""Checks mirv_pov's signatures against the installed CS2, without starting the game.

Usage: python misc/mirv_pov_sigcheck.py [--cs2 <path to "Counter-Strike Global Offensive">]

1. Every FindPatternString pattern in AfxHookSource2/MirvPov.cpp (read from the source, so it
   stays in sync) must match exactly once in client.dll / engine2.dll.
2. The checks MirvPov_HookClientDll / MirvPov_HookEngine2Dll do on what they matched (call
   targets, referenced strings, byte layouts, derived offsets) are replayed here, and the derived
   offsets are printed so they can be compared with the previous run. Keep these in sync with
   MirvPov.cpp when a hook changes.

Exit code 0 when everything passes, 1 otherwise. Only needs the Python standard library.
"""

import os
import re
import struct
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MIRVPOV_CPP = os.path.join(REPO, 'AfxHookSource2', 'MirvPov.cpp')
CS2_DIR_NAME = 'Counter-Strike Global Offensive'


# --- Finding CS2 ---

def find_cs2():
    steam_paths = []
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
            steam_paths.append(winreg.QueryValueEx(key, 'SteamPath')[0])
    except OSError:
        pass
    steam_paths.append(r'C:\Program Files (x86)\Steam')
    for steam in steam_paths:
        libraries = [steam]
        vdf = os.path.join(steam, 'steamapps', 'libraryfolders.vdf')
        if os.path.isfile(vdf):
            with open(vdf, encoding='utf-8', errors='replace') as f:
                libraries += [p.replace('\\\\', '\\') for p in re.findall(r'"path"\s+"([^"]+)"', f.read())]
        for library in libraries:
            path = os.path.join(library, 'steamapps', 'common', CS2_DIR_NAME)
            if os.path.isdir(path):
                return path
    return None


# --- Minimal PE image loader (sections mapped at their RVAs) ---

class Image:
    def __init__(self, path):
        self.path = path
        data = open(path, 'rb').read()
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        if data[pe:pe + 4] != b'PE\0\0':
            raise ValueError(path + ': not a PE file')
        num_sections, opt_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from('<H', data, opt)[0] != 0x20b:
            raise ValueError(path + ': not PE32+')
        self.base = struct.unpack_from('<Q', data, opt + 24)[0]
        size = struct.unpack_from('<I', data, opt + 56)[0]
        self.img = bytearray(size)
        self.img[:0x1000] = data[:0x1000]
        self.text = []
        sec = opt + opt_size
        for i in range(num_sections):
            name = data[sec:sec + 8].rstrip(b'\0')
            vsize, va, raw_size, raw_ptr = struct.unpack_from('<IIII', data, sec + 8)
            n = min(raw_size, vsize) if vsize else raw_size
            self.img[va:va + n] = data[raw_ptr:raw_ptr + n]
            if name == b'.text':
                self.text.append((va, va + vsize))
            sec += 40
        self.img = bytes(self.img)

    def find_all(self, pattern, ranges=None):
        rx = re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in pattern.split()), re.S)
        hits = []
        for a, b in ranges or self.text:
            hits += [m.start() for m in rx.finditer(self.img, a, b)]
        return hits

    def find(self, pattern, ranges=None):
        hits = self.find_all(pattern, ranges)
        return hits[0] if hits else None

    def rel32(self, a):
        return struct.unpack_from('<i', self.img, a)[0]

    def u32(self, a):
        return struct.unpack_from('<I', self.img, a)[0]

    def ptr(self, a):
        return struct.unpack_from('<Q', self.img, a)[0] - self.base

    def cstr(self, a):
        return self.img[a:a + 128].split(b'\0')[0].decode('latin1')

    def eq(self, a, bs):
        return self.img[a:a + len(bs)] == bytes(bs)

    def vtable(self, rtti_name):
        """Like Afx::BinUtils::FindClassVtable(module, name, 0, 0)."""
        name = self.img.find(rtti_name)
        if name < 0:
            return None
        type_descriptor = name - 0x10
        for m in re.finditer(re.escape(struct.pack('<I', type_descriptor)), self.img):
            col = m.start() - 0x0c
            if col < 0 or self.u32(col) != 1 or self.u32(col + 4) != 0:
                continue
            for v in re.finditer(re.escape(struct.pack('<Q', self.base + col)), self.img):
                return v.start() + 8
        return None


# --- Part 1: every pattern in MirvPov.cpp is unique ---

def source_patterns():
    txt = open(MIRVPOV_CPP, encoding='utf-8').read()
    engine2_start = txt.find('void MirvPov_HookEngine2Dll(')
    out = []
    for m in re.finditer(r'FindPatternString\(\s*(\w+)\s*,((?:\s*"[^"]*")+)\s*\)', txt, re.S):
        pattern = ' '.join(' '.join(re.findall(r'"([^"]*)"', m.group(2))).split())
        if not re.fullmatch(r'([0-9A-Fa-f]{2}|\?\?)( ([0-9A-Fa-f]{2}|\?\?))*', pattern):
            continue
        line = txt.count('\n', 0, m.start()) + 1
        module = 'engine2' if 0 <= engine2_start < m.start() else 'client'
        out.append((line, m.group(1), module, pattern))
    return out


# --- Part 2: the hooks' own checks ---

class Checker:
    def __init__(self):
        self.results = []

    def check(self, name, ok, info=''):
        self.results.append(ok)
        print(f"  {'OK  ' if ok else 'FAIL'} {name}{': ' + info if info else ''}")

    def failed(self):
        return sum(1 for ok in self.results if not ok)


def check_client(c, chk):
    fsn = c.find('48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 83 C3 FD 88 05 ?? ?? ?? ??')
    hltv_flag = fsn + 0x10 + 3 + 6 + c.rel32(fsn + 0x15) if fsn is not None else None
    chk.check('FrameStageNotify IsHLTV cache', fsn is not None)

    o = c.find('48 89 6C 24 18 57 48 81 EC ?? ?? ?? ?? 44 8B 41 ?? 33 ED 48 8B 15')
    ok = o is not None and c.img[o + 0x6e] == 0x8B and c.img[o + 0x6f] == 0x4F
    chk.check('Observer spec_target check', ok, f'last target +{c.img[o + 0x10]:#x}, target +{c.img[o + 0x70]:#x}' if ok else '')

    s = c.find('53 56 48 83 EC 38 48 8B F1 48 8B DA 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 84 C0 74 ?? 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ??')
    chk.check('Scoreboard HLTV check', s is not None and hltv_flag is not None and s + 0x20 + 7 + c.rel32(s + 0x22) == hltv_flag)

    sp = c.find('48 89 74 24 10 57 48 83 EC 20 0F B6 F2 48 8B F9 3A 91 ?? ?? ?? ?? 74 ?? 88 91 ?? ?? ?? ?? C6 81')
    ok = sp is not None and c.rel32(sp + 0x12) == c.rel32(sp + 0x1a)
    chk.check('SetPredictable', ok, f'm_bPredictable +{c.rel32(sp + 0x12):#x}' if ok else '')

    getter = c.find('48 83 EC ?? 83 F9 FF 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 8B 08 48 63 C1 4C 8D 05 ?? ?? ?? ?? 33 D2 4D 8B 04 C0')
    site = c.find('33 C9 48 89 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 49 8B 4D 00 BA 04 00 00 00 41 B8')
    ok = getter is not None and site is not None and site + 9 + 5 + c.rel32(site + 10) == getter
    chk.check('Death panel: player_death site', ok)
    show = c.find('C6 05 ?? ?? ?? ?? 01 33 C9 F3 0F 10 40 30 F3 0F 11 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B F0 48 85 C0')
    chk.check('Death panel: Show site', ok and show is not None and show + 0x16 + 5 + c.rel32(show + 0x17) == getter)
    think = c.find('0F 8E ?? ?? ?? ?? 33 C9 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 00 48 8B CF FF 90 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B 57 10')
    chk.check('Death panel: update site', ok and think is not None and think + 8 + 5 + c.rel32(think + 9) == getter)

    d = c.find('48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 8B 59 ?? 8B 79 ?? 8B 71 ?? 8B 69 ?? 44 8B 71 ?? 44 8B 79 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 D2 48 85 C0 48 8D 48 E0')
    ok = d is not None and c.img[d + 0x60] == 0xE8 and c.cstr(d + 0x2c + 7 + c.rel32(d + 0x2f)) == 'CCSGO_HudDeathPanel'
    chk.check('Death card (SendLastKillerDamageToClient)', ok)

    chk.check('Chat log (CHudChatDelegate vtable)', c.vtable(b'.?AVCHudChatDelegate@@') is not None)

    x = c.find('48 8B D6 48 8D 4F 60 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8D 4F 60 E8 ?? ?? ?? ??')
    ok = hide = False
    if x is not None:
        observer = x + 7 + 5 + c.rel32(x + 8)
        ok = c.img[observer + 0x18e] == 0xE8 and c.img[observer + 0x199] == 0xE8
        if ok:
            get_code = observer + 0x18e + 5 + c.rel32(observer + 0x18f)
            if c.img[get_code + 0x0d] == 0xE8 and c.eq(get_code + 0x12, [0x80, 0xB8]):
                settings_getter = get_code + 0x0d + 5 + c.rel32(get_code + 0x0e)
                hide = c.eq(settings_getter, [0x48, 0x8D, 0x05]) and c.img[settings_getter + 7] == 0xC3
    chk.check('Crosshair', ok, 'hide flag ' + ('found' if hide else 'NOT found (optional)') if ok else '')

    cb = c.find('48 83 EC 28 48 8B 05 ?? ?? ?? ?? 80 78 ?? 00 74 ?? 41 8B 00 39 41 ?? 74 ?? 89 41 ?? 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? '
                '83 B8 ?? ?? ?? ?? 00 74 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 D2 48 85 C0 48 8D 48 E0 48 0F 44 CA 48 83 C4 28 E9')
    ok = False
    if cb is not None:
        report = cb + 0x4e + 5 + c.rel32(cb + 0x4f)
        ok = (c.eq(report + 0x7c, [0x4D, 0x8B, 0xA5]) and c.eq(report + 0x94, [0x41, 0x39, 0x44, 0x24])
              and c.eq(report + 0xa3, [0x48, 0x6B, 0xE8, 0x78]) and c.eq(report + 0xa7, [0x49, 0x03, 0x6C, 0x24]))
    chk.check('Round end damage report', ok,
              f'm_iRoundWinStatus +{c.rel32(cb + 0x2a):#x}, damage services +{c.rel32(report + 0x7f):#x}, list size +{c.img[report + 0x98]:#x} / memory +{c.img[report + 0xab]:#x}' if ok else '')

    a = c.find('83 7F 50 00 0F 8E ?? ?? ?? ?? 48 8B 47 48 48 8D 0D ?? ?? ?? ?? 48 85 C0 48 8D 54 24 ?? 4C 8B C3 48 0F 45 C8 '
               'F3 0F 10 41 18 F3 0F 10 49 1C F3 0F 11 44 24 ?? F3 0F 10 41 20 48 8B CD F3 0F 11 44 24 ?? F3 0F 11 4C 24 ?? E8')
    chk.check('Damage direction arcs', a is not None)

    sb = c.find('48 89 5C 24 10 57 48 83 EC 50 48 8B D9 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? 48 89 74 24 60 48 8B B3 ?? ?? ?? ?? '
                '48 85 F6 0F 84 ?? ?? ?? ?? 48 8B 08 45 33 C0 0F 29 74 24 40 33 D2 0F 29 7C 24 30 44 0F 29 44 24 20 4C 8B 89 ?? ?? ?? ?? 48 8B C8 41 FF D1')
    chk.check('Scope blur update', sb is not None, f'm_pCameraServices +{c.rel32(sb + 0x26):#x} (compared with the schema in game)' if sb is not None else '')

    pr = c.find('4C 8D 0D ?? ?? ?? ?? 4C 89 7C 24 20 44 8B C3 48 8D 95 ?? ?? ?? ?? 49 8B CD E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 40 38 70 58')
    lo = c.find('48 8B 0D ?? ?? ?? ?? 4C 8D 3D ?? ?? ?? ?? 49 8B D7 48 8B 01 4C 8B 40 78 48 8B 85 ?? ?? ?? ?? 48 85 C0 48 0F 45 D0 41 FF D0')
    ok = pr is not None and lo is not None and c.cstr(pr + 7 + c.rel32(pr + 3)) == '%s'
    chk.check('Chat print + localize', ok, f'ILocalize::Find index {c.img[lo + 0x17] // 8}' if ok else '')

    sn = c.find('33 C0 48 C7 44 24 34 00 00 80 3F 48 89 44 24 40 48 8D 54 24 20 B8 64 00 00 00 C7 44 24 3C FF FF FF FF 66 89 44 24 48 '
                'B9 FF FF FF FF 0F B6 44 24 4A 24 E1 0C 01 88 44 24 4A 48 8D 05 ?? ?? ?? ?? 48 89 44 24 20 E8')
    chk.check('Chat sound', sn is not None and c.cstr(sn + 0x39 + 7 + c.rel32(sn + 0x3c)) == 'HudChat.Message')

    ve = c.find('48 8B 0D ?? ?? ?? ?? 48 8D 94 24 ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 48 8D 54 24 20 48 63 08 48 69 C1 A8 00 00 00 '
                '48 8D 0D ?? ?? ?? ?? 48 03 C8 48 8B 01 FF 50 ??')
    chk.check('C4 screen shake', ve is not None, f'Shake index {c.img[ve + 0x36] // 8}' if ve is not None else '')

    ap = c.find('8B 41 48 48 8B D1 39 05 ?? ?? ?? ?? 75 ?? 48 8B 0D ?? ?? ?? ?? 45 33 C9 F3 0F 10 52 50 8B 52 4C 48 8B 01 48 FF A0 ?? ?? ?? ?? '
                '39 05 ?? ?? ?? ?? 75 ?? 48 8B 0D ?? ?? ?? ?? 44 8B 42 54 8B 52 4C 48 8B 01 48 FF A0 ?? ?? ?? ?? '
                '39 05 ?? ?? ?? ?? 75 ?? 48 8B 0D ?? ?? ?? ?? 8B 52 4C 48 8B 01 48 FF A0')
    ok = ap is not None and c.u32(ap + 0x4a + 6 + c.rel32(ap + 0x4c)) == 0x92e1d607
    chk.check('Ear ringing muffling (audio parameter)', ok, f'TriggerControl index {c.rel32(ap + 0x62) // 8}' if ok else '')

    st = c.find('40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 80 78 ?? 00 0F 85 ?? ?? ?? ?? 33 C9 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? '
                '80 B8 ?? ?? ?? ?? 00 74 ?? 0F B6 05 ?? ?? ?? ?? EB ?? 0F B6 80')
    ok = st is not None and hltv_flag is not None and st + 0x2f + 7 + c.rel32(st + 0x32) == hltv_flag
    chk.check('Spectator tools (x-ray) check', ok, f'm_bIsHLTV +{c.rel32(st + 0x3b):#x}' if ok else '')

    vt = c.vtable(b'.?AVCMsgSosStartSoundEvent@@')
    ok = False
    info = 'vtable not found'
    if vt is not None:
        serialize, parse = c.ptr(vt + 11 * 8), c.ptr(vt + 9 * 8)
        rng = [(serialize, serialize + 0x200)]
        h = c.find('8B 4E ?? C6 02 15', rng)
        so = c.find('48 63 46 ?? 4C 8D 42 01 C6 02 18', rng)
        prologue = [0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x41, 0x56, 0x41, 0x57]
        ok = h is not None and so is not None and c.eq(parse, prologue)
        # The has-bits (+0x10) tested before writing each field: 4 = hash, 0x20 = source entity.
        has_bits = (c.find('40 F6 C7 04', [(serialize, h)]) is not None and c.find('40 F6 C7 20', [(h, so)]) is not None) if ok else False
        info = f'hash +{c.img[h + 2]:#x}, source +{c.img[so + 3]:#x}, has-bits {"as expected" if has_bits else "CHANGED"}' if ok else 'layout changed'
        ok = ok and has_bits
    chk.check('Enemy radio mute (CMsgSosStartSoundEvent)', ok, info)

    sg = c.find('48 89 5C 24 18 56 48 83 EC 30 48 8B D9 48 8D 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 45 33 C9 45 33 C0 48 8B 01 FF 50 30 48 8B F0 48 85 C0')
    ok = (sg is not None and c.cstr(sg + 0x0d + 7 + c.rel32(sg + 0x10)) == 'spec_target_updated'
          and c.eq(sg + 0xbf, [0x48, 0x8B, 0xB9]) and c.rel32(sg + 0xc2) % 8 == 0)
    chk.check('spec_target_updated sender', ok, f'IGameEvent::SetEntity index {c.rel32(sg + 0xc2) // 8}' if ok else '')

    se = c.find('40 55 53 57 41 56 41 57 48 8B EC 48 81 EC ?? ?? ?? ?? 48 63 FA 4C 8B F1 41 BF FF FF FF FF 83 FF FF ?? ?? '
                '48 8B 0D ?? ?? ?? ?? 48 8D 55 ?? 44 8B C7 48 8B 01 FF 90 ?? ?? ?? ?? 8B 18 EB ?? 41 8B DF '
                '83 3D ?? ?? ?? ?? 00 74 ?? 8B 05 ?? ?? ?? ?? 83 F8 FF 8D 58 FF')
    ok = (se is not None and c.eq(se + 0x87, [0x48, 0x8D, 0x0D])
          and c.eq(se + 0x8e, [0x33, 0xF6, 0x4C, 0x89, 0x34, 0xF9, 0x41, 0xC6, 0x86]) and c.img[se + 0x9b] == 1)
    chk.check('CORE: local player setter (POV needs this)', ok, f'm_bIsLocalPlayerController +{c.rel32(se + 0x97):#x}' if ok else '')


def check_engine2(e, chk):
    sk = e.find('40 55 56 41 54 41 55 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 4C 8B 2D ?? ?? ?? ?? 45 8B E0 41 D1 EC 44 8B FA 41 80 E4 01 48 8B F1 41 F6 C0 01')
    chk.check('CORE: CDemoPlayer::SkipToTick (seeking in POV)', sk is not None)
    hl = e.find('48 8B 05 ?? ?? ?? ?? 48 85 C0 74 15 80 B8 ?? ?? ?? ?? 00 75 09 83 B8 ?? ?? ?? ?? 00 7E 03 B0 01 C3 32 C0 C3')
    chk.check('CORE: CEngineClient::IsHLTV', hl is not None)


def main():
    args = sys.argv[1:]
    cs2 = args[args.index('--cs2') + 1] if '--cs2' in args else find_cs2()
    if not cs2 or not os.path.isdir(cs2):
        print('CS2 not found, pass --cs2 <path to "' + CS2_DIR_NAME + '">')
        return 1
    dlls = {
        'client': os.path.join(cs2, 'game', 'csgo', 'bin', 'win64', 'client.dll'),
        'engine2': os.path.join(cs2, 'game', 'bin', 'win64', 'engine2.dll'),
    }
    images = {}
    for name, path in dlls.items():
        print(f"{name}.dll: {time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(path)))}, {os.path.getsize(path)} bytes")
        images[name] = Image(path)

    print('\nSignatures in MirvPov.cpp (must match exactly once):')
    chk = Checker()
    for line, rng, module, pattern in source_patterns():
        if rng != 'textRange':
            continue  # searched inside a small function, checked below
        hits = images[module].find_all(pattern)
        chk.check(f'MirvPov.cpp:{line} ({module})', len(hits) == 1, f'{len(hits)} matches' if len(hits) != 1 else '')

    print('\nHook checks (as done when the game loads):')
    check_client(images['client'], chk)
    check_engine2(images['engine2'], chk)

    failed = chk.failed()
    print(f"\n{len(chk.results) - failed}/{len(chk.results)} OK" + ('' if not failed else f', {failed} FAILED (lines with "CORE" disable POV, others one feature)'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
