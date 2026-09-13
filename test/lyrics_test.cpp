#include "lyrics.h"
#include <cassert>
int main() {
  Lyrics l;
  l.parse("[ar:Artist]\n[offset:100]\n[00:02.25][00:01.000]Hello\r\n[00:03]World\n[99:99]bad\n");
  assert(l.lines.size() == 3);
  assert(l.index(899) == -1);
  assert(l.index(900) == 0);
  assert(l.lines[0].text == "Hello");
  assert(l.index(2150) == 1);
  assert(l.index(2900) == 2);
  l.parse("[00:00.001]A\n[00:00.001]B\n[broken\n");
  assert(l.lines.size() == 2);
  assert(l.index(1) == 1);
  l.parse("[00:nan]bad\n[00:inf]bad\n[9999999999999999999999:00]bad\n");
  assert(l.lines.empty());
  l.parse("\xef\xbb\xbf[00:00]Tiếng Việt\n");
  assert(l.lines.size() == 1);
  assert(l.lines[0].text == "Tiếng Việt");
  l.parse("[1000000:00]Long");
  assert(l.lines[0].ms == 60000000000LL);
  std::string many;
  for (int i = 0; i < 1000; ++i)
    many += "[00:00]" + std::string(200, 'a') + "\n";
  l.parse(many);
  size_t bytes = 0;
  for (const auto &line : l.lines)
    bytes += line.text.size();
  assert(bytes <= 16384);
  assert(l.lines.size() <= 600);
  l.parse("");
  assert(l.index(100) == -1);
}
