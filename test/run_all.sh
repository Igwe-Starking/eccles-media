#!/bin/bash
# Host regression suite for eccles_media. Needs: gcc, python3 (numpy + Pillow), ffmpeg/ffprobe.
# Cross-checks every codec/container against independent reference implementations
# (libjpeg via Pillow, libpng via Pillow, ffmpeg). Run from anywhere:  bash test/run_all.sh
set -u
cd "$(dirname "$0")/.."
CC="gcc -std=c99 -O2 -Wall -Wextra -Iinclude -Isrc"
SRC="src/em_io.c src/em_wav.c src/em_png.c src/em_jpeg.c src/em_avi.c src/em_mp4.c src/em_demux.c src/em_mp3.c src/em_player.c"
FAILS=0; PASS=0
ok()   { PASS=$((PASS+1)); echo "  PASS  $1"; }
bad()  { FAILS=$((FAILS+1)); echo "  FAIL  $1"; }
step() { echo; echo "== $1"; }

step "build (warnings are errors for this check)"
for t in t_wav t_png t_jpeg t_media t_player t_mem; do
  extra=""; [ $t = t_wav ] && extra="-lm"; [ $t = t_media ] && extra="-lm"; [ $t = t_player ] && extra="-lm"; [ $t = t_mem ] && extra="-lm"
  if $CC -Werror test/$t.c $SRC $extra -o /tmp/$t 2>/tmp/build_$t.log; then ok "build $t"; else bad "build $t"; cat /tmp/build_$t.log; fi
done
if $CC -Werror -DEM_WITH_MINIMP3 -Itest/mock test/t_mp3.c src/em_io.c src/em_mp3.c -o /tmp/t_mp3 2>/tmp/build_mp3.log; then ok "build t_mp3 (mock minimp3)"; else bad "build t_mp3"; cat /tmp/build_mp3.log; fi

step "WAV / IMA-ADPCM"
ffmpeg -v error -y -f lavfi -i "sine=frequency=440:duration=1:sample_rate=8000" -c:a pcm_u8 -ac 1 /tmp/u8.wav
/tmp/t_wav /tmp/u8.wav /tmp/u8.raw | grep -q "WAV: all ok" && ok "PCM16 round-trip exact, seek, IMA SNR > 30 dB" || bad "wav unit"
ffmpeg -v error -y -i /tmp/u8.wav -f s16le -ac 1 /tmp/u8_ff.raw && cmp -s /tmp/u8.raw /tmp/u8_ff.raw && ok "PCM8 decode == ffmpeg" || bad "PCM8 vs ffmpeg"
ffmpeg -v error -y -i /tmp/t_ima.wav -f s16le -ac 2 /tmp/ima_ff.raw && [ -s /tmp/ima_ff.raw ] && ok "our IMA-ADPCM WAV decodes in ffmpeg" || bad "IMA in ffmpeg"

step "PNG (vs Pillow/libpng, ffmpeg)"
out=$(python3 test/png_check.py 2>&1); n=$(echo "$out" | grep -c '^ok'); echo "$out" | grep -q "FAIL" && bad "png cases" ; [ "$n" -ge 23 ] && ok "png: $n individual cases ok" || bad "png: only $n cases ran (expected >= 23)"
echo "$out" | grep -q "DECODE: all ok" && ok "PNG decode (RGB/RGBA/gray/palette/tRNS/1-2-4-16 bit, all filters)" || bad "PNG decode"
echo "$out" | grep -q "ENCODE: all ok" && ok "PNG encode decodes exactly in Pillow + ffmpeg" || bad "PNG encode"

step "JPEG (vs libjpeg via Pillow, ffmpeg)"
out=$(python3 test/jpeg_check.py 2>&1); n=$(echo "$out" | grep -c '^ok'); echo "$out" | grep -q "FAIL" && bad "jpeg cases"; [ "$n" -ge 20 ] && ok "jpeg: $n individual cases ok" || bad "jpeg: only $n cases ran (expected >= 20)"
echo "$out" | grep -q "DECODE: all ok" && ok "JPEG decode (4:4:4/4:2:2/4:2:0/gray/restart/odd sizes/scales, fuzz: no crashes)" || bad "JPEG decode"
echo "$out" | grep -q "ENCODE: all ok" && ok "JPEG encode: quality == libjpeg, size within 5%, ffmpeg-clean" || bad "JPEG encode"

step "Containers (AVI / MP4 / 3GP) vs ffmpeg"
for f in avi mp4 3gp; do /tmp/t_media mux $f /tmp/m_ours.$f 1 >/dev/null && ok "mux $f" || bad "mux $f"; done
/tmp/t_media mux avi /tmp/m_ours_st.avi 2 >/dev/null; /tmp/t_media mux mp4 /tmp/m_ours_st.mp4 2 >/dev/null
for f in m_ours.avi m_ours.mp4 m_ours.3gp m_ours_st.avi m_ours_st.mp4; do
  [ -z "$(ffmpeg -v error -i /tmp/$f -f null - 2>&1)" ] && ok "ffmpeg decodes $f with zero errors" || bad "ffmpeg decode $f"
done
for f in avi mp4 3gp; do /tmp/t_media mux $f /tmp/m_ours.$f 1 >/dev/null; /tmp/t_media demux /tmp/m_ours.$f /tmp/dm_$f >/dev/null
  cmp -s /tmp/dm_$f.mjpg /tmp/mux_ref.mjpg && cmp -s /tmp/dm_$f.pcm /tmp/mux_ref.pcm && ok "mux->demux $f payloads byte-identical" || bad "roundtrip $f"; done
ffmpeg -v error -y -f lavfi -i "testsrc=size=176x144:rate=12:duration=2" -f lavfi -i "sine=frequency=500:sample_rate=22050:duration=2" -c:v mjpeg -q:v 4 -c:a pcm_s16le -ac 2 /tmp/ff.avi
ffmpeg -v error -y -f lavfi -i "testsrc=size=176x144:rate=12:duration=2" -f lavfi -i "sine=frequency=500:sample_rate=22050:duration=2" -c:v mjpeg -q:v 4 -c:a pcm_s16le -ac 1 /tmp/ff.mov
for f in ff.avi ff.mov; do /tmp/t_media demux /tmp/$f /tmp/dff >/dev/null
  ffmpeg -v error -y -i /tmp/$f -map 0:v -c copy -f mjpeg /tmp/ref_v.mjpg; ffmpeg -v error -y -i /tmp/$f -map 0:a -c copy -f s16le /tmp/ref_a.pcm
  cmp -s /tmp/dff.mjpg /tmp/ref_v.mjpg && cmp -s /tmp/dff.pcm /tmp/ref_a.pcm && ok "demux ffmpeg-made $f == ffmpeg stream copy" || bad "demux $f"; done
ffmpeg -v error -y -f lavfi -i "testsrc=size=176x144:rate=12:duration=2" -f lavfi -i "sine=frequency=500:sample_rate=22050:duration=2" -c:v libx264 -c:a aac /tmp/h264.mp4 2>/dev/null
/tmp/t_media demux /tmp/h264.mp4 /tmp/dx | grep -q "vc=2.*ac=3" && ok "H.264/AAC mp4 parsed, flagged pass-through-only" || bad "h264 detection"
ffmpeg -v error -y -f lavfi -i "testsrc=size=176x144:rate=12:duration=2" -an -c:v mjpeg -q:v 4 /tmp/vonly.avi

step "Player"
/tmp/t_player | tee /tmp/player.log | grep -E "FAIL|PLAYER" ; grep -q "PLAYER: all ok" /tmp/player.log && ok "player (stills, wav, adpcm, avi/mp4/3gp, scaling, A/V pacing, late-frame drop, junk refused)" || bad "player"

step "MP3 adapter (framing logic with a MOCK decoder; real minimp3 not available offline)"
/tmp/t_mp3 | grep -q "ok" && ok "ID3 skip, refill, resync over junk, 200/200 frames" || bad "mp3 adapter"

echo; echo "$PASS passed, $FAILS failed"; [ $FAILS -eq 0 ]
