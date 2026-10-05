# compat/

The POSIX pieces MuPDF expects and vbcc's AmigaOS libc does not have. Only
`compat.c` has code in it, and that code runs on dos.library.

| header | provides | used by |
|---|---|---|
| `sys/types.h` | `off_t`, `ssize_t`, `mode_t`, `pid_t` | several |
| `unistd.h` | `unlink`, `ftruncate` (always fails); `fseeko`/`ftello` as `fseek`/`ftell`; `fileno` as 0 | temp files, output |
| `sys/stat.h` | `stat` (Lock/Examine), `mkdir` (CreateDir) | `fz_is_directory`, time stamps |
| `sys/time.h` | `gettimeofday` (DateStamp, 1/50 s) | `random.c` seeding |
| `dirent.h` | `opendir`/`readdir`/`closedir` (Examine/ExNext) | directory "documents" |
| `fcntl.h` | the `O_*` flags; `open` (always fails) | zlib's `gzguts.h`, no gz* file is built |

`compat.c` also supplies these, which the vbcc libc lacks:

- `strnlen` and `timegm`.
- `random`/`srandom` (xorshift).
- `getentropy`. This is **not** a cryptographic source. MuPDF uses it only for file IDs and
  AES IVs when it writes a PDF, which a viewer never does.

Every one of these sits off the rendering path.
