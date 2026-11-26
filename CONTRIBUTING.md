# Contributing to ECCLES MEDIA

## Getting set up

You need a C99 compiler and `make`. The test suite also needs Python 3 (NumPy, Pillow) and ffmpeg.

```bash
make examples     # build the host example
make test         # full regression suite
```

## Guidelines

- Keep `src/` portable plain C99. Anything platform specific belongs in `port/`.
- Stream: do not hold whole frames or files in the codecs.
- Add or update tests in `test/` for behaviour changes.
- If you tested on real hardware, say which board and SDK version in the pull request.
- Update `CHANGELOG.md` for user-visible changes.

## Reporting bugs

Open an issue with the platform, compiler, the `em_err_t` returned and the smallest file that reproduces it.
