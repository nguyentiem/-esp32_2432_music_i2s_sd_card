#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
struct Lyric {
  int64_t ms;
  std::string text;
};
class Lyrics {
public:
  std::vector<Lyric> lines;
  void parse(const std::string &data) {
    lines.clear();
    int64_t offset = 0;
    size_t textBytes = 0;
    size_t pos = data.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    while (pos < data.size()) {
      size_t end = data.find('\n', pos);
      if (end == std::string::npos)
        end = data.size();
      std::string s = data.substr(pos, end - pos);
      pos = end + 1;
      if (!s.empty() && s.back() == '\r')
        s.pop_back();
      if (s.compare(0, 8, "[offset:") == 0) {
        offset = std::max<int64_t>(
            -86400000, std::min<int64_t>(86400000, std::strtoll(s.c_str() + 8, nullptr, 10)));
        continue;
      }
      std::vector<int64_t> times;
      size_t p = 0;
      while (p < s.size() && s[p] == '[') {
        size_t close = s.find(']', p);
        if (close == std::string::npos)
          break;
        std::string tag = s.substr(p + 1, close - p - 1);
        char *q = nullptr;
        long m = std::strtol(tag.c_str(), &q, 10);
        if (q == tag.c_str() || *q != ':' || m < 0 || m > 1000000)
          break;
        char *r = nullptr;
        double sec = std::strtod(q + 1, &r);
        if (r == q + 1 || *r || !std::isfinite(sec) || sec < 0 || sec >= 60)
          break;
        times.push_back(int64_t(m) * 60000 + static_cast<int64_t>(sec * 1000 + 0.5));
        p = close + 1;
      }
      size_t length = std::min<size_t>(s.size() - p, 191);
      while (length && p + length < s.size() && (uint8_t(s[p + length]) & 0xc0) == 0x80)
        --length;
      for (auto t : times) {
        if (lines.size() >= 600 || textBytes + length > 16384)
          break;
        lines.push_back({t, s.substr(p, length)});
        textBytes += length;
      }
    }
    for (auto &l : lines)
      l.ms -= offset;
    std::stable_sort(lines.begin(), lines.end(),
                     [](const Lyric &a, const Lyric &b) { return a.ms < b.ms; });
  }
  int index(int64_t ms) const {
    auto it = std::upper_bound(lines.begin(), lines.end(), ms,
                               [](int64_t t, const Lyric &l) { return t < l.ms; });
    return static_cast<int>(it - lines.begin()) - 1;
  }
};
