#!/usr/bin/env python3
"""Download files from the Amiga over FTP, in binary mode.

  amiget.py <amiga path> [<amiga path> ...] [-o <local dir>]

  amiget.py ram:wpdf-005.txt
  amiget.py "ram:p*.ppm" -o /tmp/pages       (a pattern in the name part is matched on the Amiga)

An Amiga path is split at its last ':' or '/'.  The directory part is what the server is
asked to CWD to ("RAM:", "SYS:WarpApps"), and the rest is the file name, or an fnmatch
pattern matched case-insensitively, as AmigaDOS matches names.  Files land in <local dir>,
default ./temp in this project (git-ignored).  Each download is checked against the server's
SIZE where the server reports one.

Credentials come from the AMIGA_FTP_* environment variables, or else from ftp_config.local
next to this script: shell-style KEY=value lines, git-ignored, never committed (see
ftp_config.local.example).
  AMIGA_FTP_HOST, AMIGA_FTP_USER, AMIGA_FTP_PASSWORD - required
  AMIGA_FTP_PORT (21), AMIGA_FTP_PASSIVE (no - the Amiga servers here are used in active mode)
"""
import fnmatch
import ftplib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(os.path.dirname(HERE), 'temp')


def load_config():
    cfg = {}
    path = os.path.join(HERE, 'ftp_config.local')
    if os.path.exists(path):
        for line in open(path):
            m = re.match(r'\s*(?:export\s+)?(AMIGA_FTP_\w+)\s*=\s*(.*?)\s*$', line)
            if m:
                cfg[m.group(1)] = m.group(2).strip('"\'')
    for k, v in os.environ.items():
        if k.startswith('AMIGA_FTP_'):
            cfg[k] = v
    missing = [k for k in ('AMIGA_FTP_HOST', 'AMIGA_FTP_USER', 'AMIGA_FTP_PASSWORD') if not cfg.get(k)]
    if missing:
        sys.exit('amiget: set %s (environment or %s)' % (', '.join(missing), path))
    return cfg


def split_amiga(path):
    i = max(path.rfind(':'), path.rfind('/'))
    if i < 0:
        return '', path
    d = path[:i + 1] if path[i] == ':' else path[:i]
    return d, path[i + 1:]


def connect(cfg):
    ftp = ftplib.FTP()
    ftp.set_pasv(cfg.get('AMIGA_FTP_PASSIVE', 'no').lower() in ('1', 'yes', 'true', 'on'))
    ftp.connect(cfg['AMIGA_FTP_HOST'], int(cfg.get('AMIGA_FTP_PORT', '21')), timeout=30)
    ftp.login(cfg['AMIGA_FTP_USER'], cfg['AMIGA_FTP_PASSWORD'])
    ftp.voidcmd('TYPE I')     # binary: ASCII mode would rewrite line ends
    return ftp


def fetch(ftp, amiga_path, out_dir):
    d, name = split_amiga(amiga_path)
    if d:
        ftp.cwd(d)
    if any(c in name for c in '*?['):
        listing = ftp.nlst()
        names = [n for n in listing if fnmatch.fnmatch(n.lower(), name.lower())]
        if not names:
            print('amiget: nothing matches %s' % amiga_path)
            return 0
    else:
        names = [name]
    got = 0
    for n in names:
        local = os.path.join(out_dir, n)
        with open(local, 'wb') as fh:
            ftp.retrbinary('RETR %s' % n, fh.write)
        size = os.path.getsize(local)
        try:
            remote = ftp.size(n)
        except ftplib.all_errors:
            remote = None
        check = '' if remote is None else (' (size matches)' if remote == size else ' (SIZE MISMATCH: server says %d)' % remote)
        print('%s%s -> %s, %d bytes%s' % (d, n, local, size, check))
        got += 1
    return got


def main():
    args = sys.argv[1:]
    out_dir = DEFAULT_OUT
    if '-o' in args:
        i = args.index('-o')
        out_dir = args[i + 1]
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    os.makedirs(out_dir, exist_ok=True)
    cfg = load_config()
    ftp = connect(cfg)
    try:
        total = 0
        for a in args:
            total += fetch(ftp, a, out_dir)
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            ftp.close()
    sys.exit(0 if total else 1)


if __name__ == '__main__':
    main()
