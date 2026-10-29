#include "filter/chain.h"

namespace l7gate::filter {

void FilterChain::add(std::unique_ptr<RequestFilter> filter) noexcept {
  if (filter == nullptr) return;  // 空 unique_ptr 不进链：一条链里有 nullptr，
                                  // run 就要在每个请求上多判一次，而那件事本该在这里做完。
  filters_.push_back(std::move(filter));
}

std::string_view FilterChain::name_at(std::size_t index) const noexcept {
  if (index >= filters_.size()) return {};
  return filters_[index]->name();
}

Decision FilterChain::run(RequestInfo& info) noexcept {
  // 正序，且不跳过任何一条：这条循环的全部意义就是"顺序由添加决定"，
  // 所以它不接受任何排序输入（见头文件里"没有 priority 字段"那条）。
  // info 按可写引用传下去（S3-01）：filter 写回 target/status，第一个短路之后
  // 后面的 filter 不再有机会写——调用方据此判断"这次写回是谁写的"。
  for (const std::unique_ptr<RequestFilter>& filter : filters_) {
    if (filter->on_request(info) == Decision::kStop) return Decision::kStop;
  }
  return Decision::kContinue;
}

}  // namespace l7gate::filter
