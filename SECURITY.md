# Security Policy

ECCLES MEDIA parses image, audio and video files. The JPEG decoder is exercised with corrupted and
truncated input; the PNG, WAV, MP3 and container parsers are **not** fuzzed. Treat files from untrusted
sources with care.

## Reporting a vulnerability

If a file crashes the host or causes an out-of-bounds read or write, please report it privately through
GitHub's **Security → Report a vulnerability** on this repository. Include the file, the build
configuration and the platform.
