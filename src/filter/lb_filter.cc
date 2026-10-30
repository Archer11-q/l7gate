#include "filter/lb_filter.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "http/request_parser.h"

namespace l7gate::filter {
namespace {

// 配置坏了的表态码。**502 而不是 404**：404 说的是"你要的东西我没有路由"，
// 这一格说的是"有路由、路由指的那个组里选不出后端"——后者是网关自己的毛病。
// 它必须是 `http::gateway_error_reply()` 表里有货的数（同 RouteFilter 那条约束）。
constexpr std::uint16_t kBrokenConfigStatus = 502;

// 大小写不敏感找第一个命中的头（HTTP 字段名的语义），口径与 `ParsedRequest::find_header` 一致。
std::optional<std::string_view> find_header(std::span<const l7gate::http::HeaderEntry> headers,
                                           std::string_view name) noexcept {
  for (const l7gate::http::HeaderEntry& entry : headers) {
    if (entry.name.size() != name.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < name.size(); ++i) {
      const char a = static_cast<char>(::tolower(static_cast<unsigned char>(entry.name[i])));
      const char b = static_cast<char>(::tolower(static_cast<unsigned char>(name[i])));
      if (a != b) {
        same = false;
        break;
      }
    }
    if (same) return entry.value;
  }
  return std::nullopt;
}

}  // namespace

LoadBalanceFilter::LoadBalanceFilter(const route::GroupTable& table,
                                     std::shared_ptr<route::LbState> state) noexcept
    : table_(table), state_(std::move(state)) {}

Decision LoadBalanceFilter::on_request(RequestInfo& info) {
  if (info.upstream_group == route::kNoUpstreamGroup) {
    return Decision::kContinue;  // 没人决定过组：交给 reactor 的默认目标，这一环不表态
  }
  const route::UpstreamGroup* group = table_.group_at(info.upstream_group);
  if (group == nullptr || group->targets.empty() || state_ == nullptr) {
    info.reject_status = kBrokenConfigStatus;
    return Decision::kStop;
  }

  std::optional<std::uint16_t> picked;
  switch (group->balance) {
    case route::Balance::kRoundRobin: {
      const std::size_t offset = state_->advance_rr(info.upstream_group, group->targets.size());
      picked = group->targets[offset];
      break;
    }
    case route::Balance::kLeastConnections: {
      // 只比"当前配给下游的连接条数"，不比在飞请求数——尺子的选择与理由在 ADR-19 第四条
      // （nginx 的 least_conn 数的也是 active connections，而 ADR-15 的配对是下游存活期独占）。
      // 等长时取成员顺序的第一条：这条破平规则让"两个后端一样闲"这件事有唯一答案，
      // 否则选择顺序由 vector 的历史决定，用例里那条方向断言会变成随机红。
      std::uint16_t best = group->targets[0];
      std::uint32_t best_load = state_->pairings(best);
      for (const std::uint16_t target : group->targets) {
        const std::uint32_t load = state_->pairings(target);
        if (load < best_load) {
          best = target;
          best_load = load;
        }
      }
      picked = best;
      break;
    }
    case route::Balance::kConsistentHash: {
      picked = route::select_from_ring(*group, hash_key_of(info, group->hash_key));
      break;
    }
  }
  if (!picked.has_value()) {  // 环没建（装配漏调 build_hash_ring）或建环失败
    info.reject_status = kBrokenConfigStatus;
    return Decision::kStop;
  }
  info.upstream_target = *picked;
  return Decision::kContinue;
}

std::string_view LoadBalanceFilter::hash_key_of(const RequestInfo& info,
                                                const route::HashKey& key) const noexcept {
  if (key.source == route::HashKey::Source::kHeaderName && !key.header_name.empty()) {
    const std::optional<std::string_view> value = find_header(info.headers, key.header_name);
    if (value.has_value() && !value->empty()) return *value;
    // 头缺失或值为空 → 退回整条 target（理由写在头文件：空串会把所有没带头的请求钉在同一后端）。
  }
  return info.target;
}

}  // namespace l7gate::filter
