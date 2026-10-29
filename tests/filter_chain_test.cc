// S1-08：filter 链骨架的单元测试。
//
// S1-08 交付的是骨架而不是拦截，所以这里断言的是骨架那四条会被后来的改动磨掉的性质：
//   一、顺序 = 添加顺序，而且添加顺序我故意排成一个既不是字典序、也不是"注册顺序"
//        的形状（c、a、b）——如果实现里混进了任何排序，这个用例当场就红，而
//        "a、b、c" 那种样本连"它是不是反了"都测不出来（除非恰好少一个）。
//   二、第一个 kStop 之后的 filter 一次都不被调用（链的短路语义，S4 的依赖项）。
//   三、filter 的**状态**跨请求存活（unique_ptr 持有 + 链不复制它）——限流窗口、
//        熔断健康度都建在这条上；这条一崩，S4 的所有实现同时崩。
//   四、空链就是 kContinue，主路径不为"有没有 filter"分叉。
// S3-01 起了第一个业务 filter（路由）并给链加了一条写回通道，所以这里多了第五条：
//   五、写回通道只在**真的跑到那一环**时才被写：短路之后的 filter 不许留下它的痕迹
//        （否则"链停下了"和"某个没跑到的 filter 改了目的地"会同时成立）。
// 转发主路径那一处调用点的行为（拦下来就不回源）在 reactor_test.cc 的
// `S1_08_FilterReactor` 里，理由和 S1-06/S1-07 一样：需要真 socket 与事件循环的
// 断言不放在纯函数文件里，纯函数文件也不冒充自己有连接。
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "filter/chain.h"
#include "route/router.h"

namespace {

using l7gate::filter::Decision;
using l7gate::filter::FilterChain;
using l7gate::filter::RequestFilter;
using l7gate::filter::RequestInfo;
using l7gate::http::HeaderEntry;

// 一份**稳定**的可见面内容：RequestInfo 里的全是 view，所以它们必须指向活到
// 断言之后的存储。这里刻意用函数局部的 static 常量而不是栈上临时串——测试对象
// 一旦悬垂，ASan 报的是"filter 存下了 view"这类我本来要防的错，而不是我的样本写错了。
const std::string kMethod = "GET";
const std::string kTarget = "/api/v1/orders?id=7";
const std::string kHost = "gateway.test";
const std::string kAccept = "*/*";

RequestInfo make_info(int fd = 17) {
  static const std::vector<HeaderEntry> headers{
      HeaderEntry{"Host", std::string_view(kHost)},
      HeaderEntry{"Accept", std::string_view(kAccept)},
  };
  return RequestInfo{
      .method = std::string_view(kMethod),
      .target = std::string_view(kTarget),
      .headers = std::span<const HeaderEntry>(headers),
      .request_bytes = 92,
      .downstream_fd = fd,
  };
}

// 记录调用顺序的标记 filter。名字就是它要留下的证据。
class MarkerFilter : public RequestFilter {
 public:
  MarkerFilter(std::string name, std::vector<std::string>* log, Decision decision = Decision::kContinue)
      : name_(std::move(name)), log_(log), decision_(decision) {}

  std::string_view name() const noexcept override { return name_; }
  Decision on_request(RequestInfo&) override {
    log_->push_back(name_);
    return decision_;
  }

 private:
  std::string name_;
  std::vector<std::string>* log_;
  Decision decision_;
};

// 跑一次链，用一个全新的 RequestInfo。
// S3-01 之前这里直接写 `run_through(chain)`；`run` 现在要的是可写引用（写回通道），
// 传不了右值。**我没有让 run 收右值**（`run(RequestInfo&&)` 会把"链可以写回"这件事
// 藏成一个看起来无害的临时对象），而是让调用方自己决定那份可写的存储在哪里。
Decision run_through(FilterChain& chain) {
  RequestInfo info = make_info();
  return chain.run(info);
}

}  // namespace

TEST(S1_08_FilterChain, EmptyChainContinuesAndAddsNothing) {
  FilterChain chain;
  EXPECT_TRUE(chain.empty());
  EXPECT_EQ(chain.size(), 0u);
  // 空链必须是"放行"，而且主路径不该为它分叉：这条断言防的是将来有人给 run()
  // 加一个"链是空的就走另一条路"的分支——那等于把骨架变成转发路径上的一个条件。
  EXPECT_EQ(run_through(chain), Decision::kContinue);
}

TEST(S1_08_FilterChain, RunOrderIsAddOrder) {
  FilterChain chain;
  std::vector<std::string> order;
  // 故意 c、a、b：见文件头第一条。字典序 / 逆序 / 任何"顺手排了一下"都会在这里现形。
  chain.add(std::make_unique<MarkerFilter>("c", &order));
  chain.add(std::make_unique<MarkerFilter>("a", &order));
  chain.add(std::make_unique<MarkerFilter>("b", &order));
  ASSERT_EQ(chain.size(), 3u);
  EXPECT_EQ(run_through(chain), Decision::kContinue);
  EXPECT_EQ(order, (std::vector<std::string>{"c", "a", "b"}));

  // name_at 是配置顺序的旁证（也是日志要用的那张表）：它必须和刚才跑过的次序一致，
  // 否则"执行顺序 == 配置顺序"这句话就只在一次运行里成立。
  EXPECT_EQ(chain.name_at(0), "c");
  EXPECT_EQ(chain.name_at(1), "a");
  EXPECT_EQ(chain.name_at(2), "b");
  EXPECT_TRUE(chain.name_at(3).empty());  // 越界不越读
}

TEST(S1_08_FilterChain, FirstStopShortCircuitsLaterFilters) {
  FilterChain chain;
  std::vector<std::string> order;
  chain.add(std::make_unique<MarkerFilter>("first", &order));
  chain.add(std::make_unique<MarkerFilter>("stopper", &order, Decision::kStop));
  chain.add(std::make_unique<MarkerFilter>("never", &order));
  EXPECT_EQ(run_through(chain), Decision::kStop);
  // "never" 一次都不该被调用。S4 的限流放在认证 filter 后面时，这条就是
  // "没认证的请求不会被限流计数"的依据；反过来说，短路语义一变，限流统计当场失真。
  EXPECT_EQ(order, (std::vector<std::string>{"first", "stopper"}));
}

TEST(S1_08_FilterChain, FilterStatePersistsAcrossRequests) {
  class CountingFilter : public RequestFilter {
   public:
    std::string_view name() const noexcept override { return "counter"; }
    Decision on_request(RequestInfo&) override {
      ++seen_;
      // 第 3 条起拦下：这就是 S4 限流的形状（窗口计数器 + 阈值），今天用假阈值把它锁住。
      return seen_ >= 3 ? Decision::kStop : Decision::kContinue;
    }
    int seen() const noexcept { return seen_; }

   private:
    int seen_ = 0;
  };

  FilterChain chain;
  auto counted = std::make_unique<CountingFilter>();
  CountingFilter* witness = counted.get();
  chain.add(std::move(counted));

  EXPECT_EQ(run_through(chain), Decision::kContinue);
  EXPECT_EQ(run_through(chain), Decision::kContinue);
  EXPECT_EQ(run_through(chain), Decision::kStop);
  // 计数器走到 3 才拦得下来，证明链持有的是**同一个对象**而不是每条请求复制一份。
  // 复制一次的话这条用例的失败方式很安静：第三次的 seen_ 是 1，kContinue，
  // 而"限流不生效"这种错要到 S4 才被真实流量发现——所以它在 S1-08 就得红。
  EXPECT_EQ(witness->seen(), 3);
}

TEST(S1_08_FilterChain, RequestInfoReachesFiltersUnchanged) {
  struct Peek : public RequestFilter {
    std::string_view name() const noexcept override { return "peek"; }
    Decision on_request(RequestInfo& info) override {
      seen_method = info.method;
      seen_target = info.target;
      seen_headers = info.headers;
      seen_bytes = info.request_bytes;
      seen_fd = info.downstream_fd;
      return Decision::kContinue;
    }
    std::string_view seen_method, seen_target;
    std::span<const HeaderEntry> seen_headers;
    std::size_t seen_bytes = 0;
    int seen_fd = -1;
  };

  FilterChain chain;
  auto peek = std::make_unique<Peek>();
  Peek* witness = peek.get();
  chain.add(std::move(peek));
  // 不再是 const：`run` 现在带写回通道，可写性是接口的一部分（S3-01，ADR-18 第一节）。
  RequestInfo info = make_info(/*fd=*/42);
  ASSERT_EQ(chain.run(info), Decision::kContinue);

  EXPECT_EQ(witness->seen_method, std::string_view(kMethod));
  EXPECT_EQ(witness->seen_target, std::string_view(kTarget));
  ASSERT_EQ(witness->seen_headers.size(), 2u);
  EXPECT_EQ(witness->seen_headers[0].name, "Host");
  EXPECT_EQ(witness->seen_headers[1].value, std::string_view(kAccept));
  EXPECT_EQ(witness->seen_bytes, 92u);
  EXPECT_EQ(witness->seen_fd, 42);
  // 写回通道的**默认值**也是一条断言：读者（reactor）区分"filter 没决定"和
  // "filter 决定了一个还没被装配好的下标"，靠的就是这两个默认值本身。
  EXPECT_EQ(info.upstream_target, l7gate::route::kNoUpstreamTarget)
      << "peek 没有写 target，它必须保持 kNoUpstreamTarget（默认目标那条路的入口）";
  EXPECT_EQ(info.reject_status, 0u) << "没表态就是 0；非 0 才会走自产响应那条出口";
}

TEST(S1_08_FilterChain, NullFilterIsNotAdded) {
  FilterChain chain;
  std::vector<std::string> order;
  chain.add(std::make_unique<MarkerFilter>("real", &order));
  chain.add(nullptr);
  // 判空发生在**入队时**而不是 run 时：链里躺着 nullptr 的话，每个请求都要为它
  // 多走一次判空分支，而那件事本该在 add 里就做完。
  EXPECT_EQ(chain.size(), 1u);
  EXPECT_EQ(run_through(chain), Decision::kContinue);
  EXPECT_EQ(order, (std::vector<std::string>{"real"}));
}

// --- S3-01（ADR-18 第一节）：链的写回通道 --------------------------------------
namespace {

// 一个会写回两个字段、并且**总是放行**的 filter。它是路由的替身：
// 这里的对象是"链能不能把 filter 写的东西交到调用方手里"，不是路由语义
// （路由语义在 tests/route_test.cc，那边不需要链）。
class WritingFilter : public RequestFilter {
 public:
  WritingFilter(std::string name, std::uint16_t target, std::uint16_t status)
      : name_(std::move(name)), target_(target), status_(status) {}

  std::string_view name() const noexcept override { return name_; }
  Decision on_request(RequestInfo& info) override {
    info.upstream_target = target_;
    info.reject_status = status_;
    return Decision::kContinue;
  }

 private:
  std::string name_;
  std::uint16_t target_;
  std::uint16_t status_;
};

}  // namespace

TEST(S3_01_ChainWriteBack, TargetWrittenByFilterReachesCaller) {
  FilterChain chain;
  chain.add(std::make_unique<WritingFilter>("router", /*target=*/1, /*status=*/0));
  RequestInfo info = make_info();
  ASSERT_EQ(chain.run(info), Decision::kContinue);
  // 这条断言就是"字段和它的第一个读者同批进来"里"写"的那一半。S1-03 那次是反过来：
  // 配置写进了结构、代码没人读它，于是限制静默失效（见 request_parser.h 里
  // max_header_count 那段）。这里读的必须是**同一个对象**，不是它的一份拷贝。
  EXPECT_EQ(info.upstream_target, 1u);
  EXPECT_EQ(info.reject_status, 0u);
}

TEST(S3_01_ChainWriteBack, ShortCircuitedFilterLeavesNoTrace) {
  // 短路之后后面的 filter **一次都不被调用**（S1-08 第二条），所以在它之后写回的
  // 目标必须保持"没人写过"。这条不是多余的一遍：链一旦哪天把短路实现成"跑完再取
  // 最后一个决定"，路由就会把一条已经被拦下的请求送到某个后端——那是"拦下来了
  // 但真的回了源"，而客户端看不出来（1.16 里写的那条讲究，正是防这一格）。
  FilterChain chain;
  std::vector<std::string> order;
  chain.add(std::make_unique<MarkerFilter>("stopper", &order, Decision::kStop));
  chain.add(std::make_unique<WritingFilter>("router", /*target=*/3, /*status=*/503));
  RequestInfo info = make_info();
  ASSERT_EQ(chain.run(info), Decision::kStop);
  EXPECT_EQ(info.upstream_target, l7gate::route::kNoUpstreamTarget);
  EXPECT_EQ(info.reject_status, 0u);
  EXPECT_EQ(order, (std::vector<std::string>{"stopper"})) << "router 一次都不该被调用";
}

TEST(S3_01_ChainWriteBack, EmptyChainLeavesDefaultTargetPath) {
  // **空链必须把"用默认目标"这条路完整地留给 reactor**。这一条是"没装路由 filter 时
  // 行为逐字节等于 S2-06a"的接口层证据：容量公式 `min(N,E)×28,232`、B-007 的 100k、
  // 八份 canonic 档能不能继续引用，最终都落在"空链时这两个字段是不是这两个默认值"上。
  FilterChain chain;
  RequestInfo info = make_info();
  ASSERT_EQ(chain.run(info), Decision::kContinue);
  EXPECT_EQ(info.upstream_target, l7gate::route::kNoUpstreamTarget);
  EXPECT_EQ(info.reject_status, 0u);
}
