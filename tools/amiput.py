#!/usr/bin/env python3
"""Upload files to the Amiga over FTP, in binary mode (the twin of amiget.py).

  amiput.py <local file> [<local file> ...] -d <amiga dir>

  amiput.py build/WarpPDFView -d SYS:WarpApps

Each file keeps its name in <amiga dir>, which must exist.  The upload is
checked against the server's SIZE where the server reports one.  Credentials
as for amiget.py: the AMIGA_FTP_* environment variables, or ftp_config.local
next to this script (git-ignored; see ftp_config.local.example).

Note that an AmigaOS FTP server usually stores a new file without the E
(executable) protection bit: "protect <file> +e" on the Amiga before the
first run.
"""
import ftplib
import os
import sys

from amiget import load_config, connect


def main():
    args = sys.argv[1:]
    if '-d' not in args:
        sys.exit(__doc__)
    i = args.index('-d')
    amiga_dir = args[i + 1]
    del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    cfg = load_config()
    ftp = connect(cfg)
    ok = True
    try:
        ftp.cwd(amiga_dir)
        for local in args:
            name = os.path.basename(local)
            with open(local, 'rb') as fh:
                ftp.storbinary('STOR %s' % name, fh)
            size = os.path.getsize(local)
            try:
                remote = ftp.size(name)
            except ftplib.all_errors:
                remote = None
            if remote is not None and remote != size:
                ok = False
                print('%s -> %s/%s: SIZE MISMATCH (%d bytes sent, server says %d)' % (local, amiga_dir, name, size, remote))
            else:
                print('%s -> %s %s, %d bytes' % (local, amiga_dir, name, size))
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            ftp.close()
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
