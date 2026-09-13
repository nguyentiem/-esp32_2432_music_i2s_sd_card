#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_tmp=$(mktemp -d)
trap 'rm -rf "$test_tmp"' EXIT
for name in lyrics wav; do
  g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -Iinclude "test/${name}_test.cpp" -o "$test_tmp/$name"
  "$test_tmp/$name"
done
if command -v ffmpeg >/dev/null; then
  ffmpeg -hide_banner -loglevel error -f lavfi -i 'sine=frequency=440:duration=1:sample_rate=44100' \
    -ac 2 -codec:a libmp3lame -b:a 128k "$test_tmp/tone.mp3"
  g++ -std=c++17 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -Ithird_party/minimp3 test/mp3_test.cpp -o "$test_tmp/mp3"
  "$test_tmp/mp3" "$test_tmp/tone.mp3"
else
  echo 'MP3 test skipped: ffmpeg is not installed'
fi
