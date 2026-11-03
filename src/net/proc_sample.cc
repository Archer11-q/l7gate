#include "net/proc_sample.h"

#include <dirent.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace l7gate::net {

std::int64_t count_open_fds() {
  DIR* dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) return -1;
  std::int64_t n = 0;
  while (const dirent* entry = ::readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    ++n;
  }
  ::closedir(dir);
  return n;
}

std::int64_t current_rss_kib() {
  std::FILE* file = std::fopen("/proc/self/status", "r");
  if (file == nullptr) return -1;
  char line[256];
  std::int64_t rss = -1;
  while (std::fgets(line, sizeof(line), file)) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      rss = std::strtoll(line + 6, nullptr, 10);
      break;
    }
  }
  std::fclose(file);
  return rss;
}

}  // namespace l7gate::net
