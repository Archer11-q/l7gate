#include "route/config_snapshot.h"

namespace l7gate::route {
namespace {

// 世代对象的构造/析构计数。放在文件作用域而不是 `ConfigPublisher` 里：一个世代可能在
// 发布点之外被持有（用例钉住一根 `shared_ptr` 断"最后一个引用释放前不回收"），
// 计数必须跟着对象走，而不是跟着发布点走——与 `connections_constructed` 同一个理由。
std::atomic<std::uint64_t> snapshots_constructed{0};
std::atomic<std::uint64_t> snapshots_destructed{0};

}  // namespace

ConfigSnapshot::ConfigSnapshot() noexcept {
  snapshots_constructed.fetch_add(1, std::memory_order_relaxed);
}

ConfigSnapshot::~ConfigSnapshot() noexcept {
  snapshots_destructed.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t ConfigSnapshot::constructed() noexcept {
  return snapshots_constructed.load(std::memory_order_relaxed);
}

std::uint64_t ConfigSnapshot::destructed() noexcept {
  return snapshots_destructed.load(std::memory_order_relaxed);
}

std::uint64_t ConfigSnapshot::live() noexcept {
  return constructed() - destructed();
}

}  // namespace l7gate::route
