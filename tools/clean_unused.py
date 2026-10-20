"""Find files the game, its tools and its docs no longer use, and optionally delete them.

    python tools/clean_unused.py                  report only, nothing is touched
    python tools/clean_unused.py --list           also print every file
    python tools/clean_unused.py --delete         send the reported files to the Recycle Bin
    python tools/clean_unused.py --only screenshots,build_scratch
    python tools/clean_unused.py --include vfxgen_cache

Categories (on by default):
  screenshots    test / UI captures: root *.png / *.jpg, screenshots/, looks_screenshots/, and docs/
                 images that no doc, tool or code file names (docs dirs a tool reads are kept)
  build_scratch  what test scripts leave in build*/ and build*/bin: top-level png / jpg / log / csv /
                 wav (+ cfg in build*/ itself); never the game, its data, CMake files or caches
  old_binaries   build outputs committed at the repo root long ago (*.exe *.pdb *.ilk *.exp *.lib)
                 and the root *.log files
  assets         textures / sounds / models under assets/ that nothing names. A file counts as used
                 when its name, or its name minus a trailing number or up to two _suffix parts,
                 appears in any code / data / shader / tool / doc file: that covers the names the
                 loaders build at runtime (terrain <base>_albedo, tree <stem>_n, icons <item id>,
                 kits <id>.glb, sounds <name>_<n>.ogg, vfx <stem>_mv / _lm0)
Opt-in (--include):
  vfxgen_cache   tools/vfxgen/cache: downloaded Higgsfield generations; re-creating them costs credits

Never touched: assets/human (source art), assets/treefbx (CMake builds every file in it),
assets/cache, assets/vgeo, openable builds, .claude, .toolchain, .git.
Deleted tracked files show up in `git status` as deletions: review and commit them.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

IMAGE = {'.png', '.jpg', '.jpeg', '.tga', '.bmp', '.ktx2', '.dds', '.exr', '.hdr'}
AUDIO = {'.ogg', '.wav', '.mp3', '.flac'}
MODEL = {'.glb', '.gltf', '.fbx', '.obj', '.cube'}
TEXT = {'.c', '.h', '.cpp', '.hpp', '.m', '.mm', '.glsl', '.vert', '.frag', '.comp', '.json', '.py',
        '.cfg', '.sh', '.bat', '.ps1', '.txt', '.cmake', '.in', '.md', '.story', '.js', '.mjs', '.html',
        '.css', '.mtl', '.gltf', '.yml', '.yaml', '.toml', '.ini', '.csv', '.plist'}

# Never read for references (logs / caches would make everything look used) and never deleted.
SKIP = {'.git', '.claude', '.toolchain', 'openable builds', 'assets/cache', 'tools/vfxgen/cache',
        'tools/vfxgen/logs'}
ASSET_KEEP = ('assets/human/', 'assets/treefbx/', 'assets/cache/', 'assets/vgeo/')
SCRATCH_TOP = {'.png', '.jpg', '.bmp', '.log', '.csv', '.wav', '.cfg'}
SCRATCH_BIN = {'.png', '.jpg', '.bmp', '.log', '.csv', '.wav'}
OLD_BIN = {'.exe', '.pdb', '.ilk', '.exp', '.lib', '.log'}
DEFAULT = ['screenshots', 'build_scratch', 'old_binaries', 'assets']
OPT_IN = ['vfxgen_cache']


def rel(path):
    return os.path.relpath(path, ROOT).replace('\\', '/')


def is_link(entry):
    # Junctions too (build*/ and .claude/worktrees point to another drive): never walk through them.
    return entry.is_symlink() or (hasattr(entry, 'is_junction') and entry.is_junction())


def walk(top, skip_build=True):
    stack = [top]
    while stack:
        d = stack.pop()
        try:
            entries = list(os.scandir(d))
        except OSError:
            continue
        for e in entries:
            r = rel(e.path)
            if e.is_dir(follow_symlinks=False):
                if is_link(e) or r in SKIP or e.name in ('__pycache__', 'node_modules'):
                    continue
                if skip_build and '/' not in r and r.startswith('build'):
                    continue
                stack.append(e.path)
            elif e.is_file(follow_symlinks=False):
                yield e.path, r


def files_in(top):
    """Every file under top (a real dir, entered even when the top itself is a junction)."""
    for d, _, names in os.walk(top):
        for n in names:
            yield os.path.join(d, n)


class Corpus:
    def __init__(self):
        self.tokens = set()
        self.code_text = []   # non-doc files, for 'does a tool read this dir'
        self.all_text = []
        for path, r in walk(ROOT):
            if os.path.splitext(path)[1].lower() not in TEXT and os.path.basename(path) != 'CMakeLists.txt':
                continue
            try:
                if os.path.getsize(path) > 32 << 20:
                    continue
                with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                    t = f.read().lower().replace('\\', '/')
            except OSError:
                continue
            self.tokens.update(re.findall(r'[a-z0-9_\-]+', t))
            self.all_text.append(t)
            if not r.startswith('docs/') and not r.endswith('.md'):
                self.code_text.append(t)
        self.code_blob = '\n'.join(self.code_text)
        self.blob = '\n'.join(self.all_text)

    def names(self, stem):
        s = stem.lower()
        out = {s, re.sub(r'\d+$', '', s), re.sub(r'[_\-]?\d+$', '', s)}
        t = s
        for _ in range(2):
            t = re.sub(r'[_\-][^_\-]*$', '', t)
            out.add(t)
        return {n for n in out if len(n) >= 3}

    def used(self, path):
        stem = os.path.basename(path)
        stem = stem[:stem.index('.')] if '.' in stem else stem
        if self.names(stem) & self.tokens:
            return True
        if re.search(r'[^a-z0-9_\-]', stem.lower()):    # spaces / dots: plain substring search
            return stem.lower() in self.blob
        return False


def git_tracked():
    try:
        out = subprocess.run(['git', '-C', ROOT, 'ls-files', '-z'], capture_output=True, check=True).stdout
        return set(out.decode('utf-8', 'replace').split('\0'))
    except (OSError, subprocess.CalledProcessError):
        return set()


def find(categories):
    found = {c: [] for c in categories}
    corpus = None
    if 'screenshots' in categories or 'assets' in categories:
        print('reading code / data / docs for references ...')
        corpus = Corpus()

    if 'screenshots' in categories:
        out = found['screenshots']
        for e in os.scandir(ROOT):
            if e.is_file() and os.path.splitext(e.name)[1].lower() in IMAGE:
                out.append(e.path)
        for d in ('screenshots', 'looks_screenshots'):
            if os.path.isdir(os.path.join(ROOT, d)):
                out.extend(files_in(os.path.join(ROOT, d)))
        docs = os.path.join(ROOT, 'docs')
        for path, r in walk(docs):
            if os.path.splitext(path)[1].lower() not in IMAGE:
                continue
            parts = r.split('/')[:-1]
            read_by_tool = any('/'.join(parts[:k]) in corpus.code_blob for k in range(2, len(parts) + 1))
            if not read_by_tool and not corpus.used(path):
                out.append(path)

    if 'build_scratch' in categories:
        out = found['build_scratch']
        for e in os.scandir(ROOT):
            if not (e.is_dir() and e.name.startswith('build')):
                continue
            for f in os.scandir(e.path):
                if f.is_file() and os.path.splitext(f.name)[1].lower() in SCRATCH_TOP:
                    out.append(f.path)
            b = os.path.join(e.path, 'bin')
            if os.path.isdir(b):
                for f in os.scandir(b):
                    if f.is_file() and os.path.splitext(f.name)[1].lower() in SCRATCH_BIN:
                        out.append(f.path)

    if 'old_binaries' in categories:
        for e in os.scandir(ROOT):
            if e.is_file() and os.path.splitext(e.name)[1].lower() in OLD_BIN:
                found['old_binaries'].append(e.path)

    if 'assets' in categories:
        for path, r in walk(os.path.join(ROOT, 'assets')):
            if r.startswith(ASSET_KEEP):
                continue
            if os.path.splitext(path)[1].lower() in IMAGE | AUDIO | MODEL and not corpus.used(path):
                found['assets'].append(path)

    if 'vfxgen_cache' in categories:
        c = os.path.join(ROOT, 'tools', 'vfxgen', 'cache')
        if os.path.isdir(c):
            found['vfxgen_cache'].extend(files_in(c))
    return found


def recycle(paths):
    """Recycle Bin on Windows (undo from there); plain delete elsewhere. Returns the failures."""
    if os.name != 'nt':
        bad = []
        for p in paths:
            try:
                os.remove(p)
            except OSError:
                bad.append(p)
        return bad
    import ctypes
    from ctypes import wintypes

    class SHFILEOPSTRUCTW(ctypes.Structure):
        _fields_ = [('hwnd', wintypes.HWND), ('wFunc', wintypes.UINT), ('pFrom', ctypes.c_void_p),
                    ('pTo', ctypes.c_void_p), ('fFlags', ctypes.c_uint16), ('fAnyOperationsAborted', wintypes.BOOL),
                    ('hNameMappings', ctypes.c_void_p), ('lpszProgressTitle', ctypes.c_void_p)]
    FO_DELETE, FOF_SILENT, FOF_NOCONFIRMATION, FOF_ALLOWUNDO, FOF_NOERRORUI = 3, 0x4, 0x10, 0x40, 0x400
    for i in range(0, len(paths), 200):
        chunk = [os.path.abspath(p) for p in paths[i:i + 200]]
        buf = ctypes.create_unicode_buffer('\0'.join(chunk) + '\0\0')
        op = SHFILEOPSTRUCTW(None, FO_DELETE, ctypes.addressof(buf), None,
                             FOF_SILENT | FOF_NOCONFIRMATION | FOF_ALLOWUNDO | FOF_NOERRORUI, False, None, None)
        ctypes.windll.shell32.SHFileOperationW(ctypes.byref(op))
    return [p for p in paths if os.path.exists(p)]


def mb(n):
    return '%.1f MB' % (n / 1048576.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--delete', action='store_true', help='send the reported files to the Recycle Bin')
    ap.add_argument('--list', action='store_true', help='print every file, not only the largest')
    ap.add_argument('--only', help='comma list of categories: ' + ','.join(DEFAULT + OPT_IN))
    ap.add_argument('--include', default='', help='add opt-in categories: ' + ','.join(OPT_IN))
    a = ap.parse_args()
    cats = a.only.split(',') if a.only else DEFAULT + [c for c in a.include.split(',') if c]
    unknown = [c for c in cats if c not in DEFAULT + OPT_IN]
    if unknown:
        sys.exit('unknown category: ' + ', '.join(unknown))

    found = find(cats)
    tracked = git_tracked()
    total, everything = 0, []
    for c in cats:
        files = sorted(set(found[c]), key=lambda p: -os.path.getsize(p))
        size = sum(os.path.getsize(p) for p in files)
        ntr = sum(1 for p in files if rel(p) in tracked)
        total += size
        everything += files
        print('\n== %s: %d files, %s (%d tracked by git)' % (c, len(files), mb(size), ntr))
        for p in files if a.list else files[:12]:
            print('   %9s  %s%s' % (mb(os.path.getsize(p)), rel(p), '  [git]' if rel(p) in tracked else ''))
        if not a.list and len(files) > 12:
            print('   ... %d more (--list shows all)' % (len(files) - 12))
    print('\ntotal: %d files, %s' % (len(everything), mb(total)))
    if not a.delete:
        print('report only: re-run with --delete to send them to the Recycle Bin')
        return
    bad = recycle(everything)
    print('deleted %d files%s' % (len(everything) - len(bad), (', %d failed (in use?)' % len(bad)) if bad else ''))
    for p in bad[:20]:
        print('   failed: ' + rel(p))
    print('tracked files now show as deletions in git status: commit them when the game still runs fine')


if __name__ == '__main__':
    main()
