#include "filter/lb_filter.h"

#include <chrono>
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

// 候选集过滤的时钟。单调毫秒，与 HealthTable 的决策时间线同一个来源口径
// （Reactor 用的是它自己的 monotonic_ms()，两边都是 steady_clock，读数可比较）。
std::int64_t now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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
                                     std::shared_ptr<route::LbState> state,
                                     std::shared_ptr<const route::HealthTable> health) noexcept
    : table_(table), state_(std::move(state)), health_(std::move(health)) {}

const std::vector<std::uint16_t>& LoadBalanceFilter::candidates_for(
    const route::UpstreamGroup& group) {
  if (health_ == nullptr) return group.targets;  // 健康检查未装配：全员候选，零行为差异
  const std::int64_t now = now_ms();
  candidates_scratch_.clear();
  for (const std::uint16_t target : group.targets) {
    if (!health_->is_ejected(target, now)) candidates_scratch_.push_back(target);
  }
  if (candidates_scratch_.empty()) {
    // 全员被摘：回退全员集合。ADR-12 的立场是"故意保留一段错误来换取不自我放大"；
    // 把请求挂死在下游缓冲里是第二故障（自产错误响应归 S4），我不在这里造它。
    return group.targets;
  }
  return candidates_scratch_;
}

Decision LoadBalanceFilter::on_request(RequestInfo& info) {
  if (info.upstream_group == route::kNoUpstreamGroup) {
    return Decision::kContinue;  // 没人决定过组：交给 reactor 的默认目标，这一环不表态
  }
  const route::UpstreamGroup* group = table_.group_at(info.upstream_group);
  if (group == nullptr || group->targets.empty() || state_ == nullptr) {
    info.reject_status = kBrokenConfigStatus;
    return Decision::kStop;
  }

  const std::vector<std::uint16_t>& candidates = candidates_for(*group);
  std::optional<std::uint16_t> picked;
  switch (group->balance) {
    case route::Balance::kRoundRobin: {
      const std::size_t offset = state_->advance_rr(info.upstream_group, candidates.size());
      picked = candidates[offset];
      break;
    }
    case route::Balance::kLeastConnections: {
      // 只比"当前配给下游的连接条数"，不比在飞请求数——尺子的选择与理由在 ADR-19 第四条
      // （nginx 的 least_conn 数的也是 active connections，而 ADR-15 的配对是下游存活期独占）。
      // 破平规则是 ADR-19 追加第三条（S3-03 开工裁决①）改过的：**平手集合内轮询**，
      // 复用每组的轮询游标，不再"取成员顺序第一条"。改它的实测理由：B-013 对照臂里
      // 98.3% 的流量吸附在成员 0，而摘除/恢复必然制造平手——"恢复了"不等于
      // "恢复后接得到流量"。
      std::uint32_t best_load = state_->pairings(candidates[0]);
      std::size_t tie_count = 0;
      for (const std::uint16_t target : candidates) {
        const std::uint32_t load = state_->pairings(target);
        if (load < best_load) best_load = load;
      }
      for (const std::uint16_t target : candidates) {
        if (state_->pairings(target) == best_load) ++tie_count;
      }
      // 平手集合只有一人时游标不动（advance_rr 不被调）：独占最小值的后端
      // 必须被选中，这条由"方向"用例钉着。集合 >1 时游标推进一次。
      std::size_t offset = 0;
      if (tie_count > 1) offset = state_->advance_rr(info.upstream_group, tie_count);
      std::size_t walked = 0;
      for (const std::uint16_t target : candidates) {
        if (state_->pairings(target) != best_load) continue;
        if (walked == offset) {
          picked = target;
          break;
        }
        ++walked;
      }
      break;
    }
    case route::Balance::kConsistentHash: {
      picked = route::select_from_ring(*group, hash_key_of(info, group->hash_key), candidates);
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
