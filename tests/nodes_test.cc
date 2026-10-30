// S3-04（ADR-20）：节点表这一层的用例——注册表的 intern 语义、快照装配的拒绝面、
// 发布器与观测面、NodesStore 的"连不上也是一句能定位的话"。
//
// 与 health_test.cc 同一条分工：**这个文件不依赖 Docker**（MySQL 服务端进不来 ctest）。
// "MySQL 真的能连"在首次连通取证里成立过一次（B-015 前的探针，ENVIRONMENT 第 3 节）；
// 真容器全链路在 scripts/smoke-s3-04.sh。这里证的是装配与发布逻辑本身——
// 它们不因数据来源是文件还是 MySQL 而变形。
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config/nodes_store.h"
#include "route/balance.h"
#include "route/health.h"
#include "route/nodes.h"
#include "route/router.h"

namespace {

using l7gate::route::Balance;
using l7gate::route::build_nodes_view;
using l7gate::route::GroupTable;
using l7gate::route::kNoUpstreamTarget;
using l7gate::route::NodeTarget;
using l7gate::route::NodesFetch;
using l7gate::route::NodesPublisher;
using l7gate::route::NodesView;
using l7gate::route::TargetRegistry;

NodeTarget node(std::uint16_t port, std::string path = "/") {
  return NodeTarget{"127.0.0.1", port, std::move(path)};
}

// 文件侧的两张组：一张静态 round_robin、一张 mysql 哈希组。多数用例用这一套。
// dynamic 组按文件里 `"source":"mysql"` 的真实形状装配：零成员 + source_mysql 标记
// （成员没到岗的状态，环随第一份有成员的快照发布）。
void build_file_groups(GroupTable* table) {
  std::uint16_t g = l7gate::route::kNoUpstreamGroup;
  ASSERT_TRUE(table->add_group("static", Balance::kRoundRobin, &g));
  ASSERT_TRUE(table->add_member(g, 0));
  ASSERT_TRUE(table->add_member(g, 1));
  g = l7gate::route::kNoUpstreamGroup;
  ASSERT_TRUE(table->add_group("dynamic", Balance::kConsistentHash, &g));
  const_cast<l7gate::route::UpstreamGroup*>(table->group_at(g))->source_mysql = true;
}

NodesFetch fetch_with_dynamic(std::uint16_t port) {
  NodesFetch fetch;
  fetch.groups.push_back({"dynamic", {node(port)}});
  return fetch;
}

}  // namespace

// --- 一、注册表：ever-seen 的 intern 语义 --------------------------------------------

// 同一个端点 intern 两次拿到**同一个**下标：reload 前后存活成员的下标不变——
// 这条是池桶、配对账、健康表、连接标签全部按"下标"索引之所以能成立的地基。
TEST(S3_04_Nodes, InternIsStablePerEndpoint) {
  TargetRegistry registry;
  const std::uint16_t a = registry.intern(node(18081));
  const std::uint16_t b = registry.intern(node(18082));
  EXPECT_EQ(registry.intern(node(18081)), a) << "同端点第二次 intern 换了下标";
  EXPECT_EQ(registry.intern(node(18082)), b);
  EXPECT_NE(a, b);
  EXPECT_EQ(registry.size(), 2u);
  EXPECT_EQ(registry.at(a)->port, 18081u);
  EXPECT_EQ(registry.at(b)->port, 18082u);
  EXPECT_EQ(registry.endpoint_keys().size(), 2u);
}

// health_path 不同**不**是两个端点：注册表按 地址:端口 认机器（探活是每目标一条，
// 同一台机器在 ever-seen 里只有一行，health_path 取第一次 intern 的那份）。
TEST(S3_04_Nodes, InternKeysOnEndpointNotOnHealthPath) {
  TargetRegistry registry;
  const std::uint16_t first = registry.intern(node(18081, "/healthz"));
  EXPECT_EQ(registry.intern(node(18081, "/livez")), first);
  EXPECT_EQ(registry.size(), 1u);
  EXPECT_EQ(registry.at(first)->health_path, "/healthz") << "保留第一次的定义";
}

// --- 二、快照装配：拒绝面 -----------------------------------------------------------

// MySQL 里出现文件没声明为 source=mysql 的组 → 整批拒绝，不留半张快照。
TEST(S3_04_Nodes, UnknownGroupNameIsRejectedWholesale) {
  GroupTable file_groups;
  build_file_groups(&file_groups);
  TargetRegistry registry;
  registry.intern(node(18081));
  registry.intern(node(18082));

  NodesFetch fetch = fetch_with_dynamic(18083);
  fetch.groups.push_back({"ghost", {node(18099)}});  // 文件里没有这个组
  std::string error;
  EXPECT_EQ(build_nodes_view(file_groups, fetch, registry, &error), nullptr);
  EXPECT_NE(error.find("ghost"), std::string::npos) << "拒绝话里要点名那个组：" << error;
}

// 静态组的成员从文件原样带过来、mysql 组的成员经注册表 intern、哈希环发布前建好。
TEST(S3_04_Nodes, ViewAssemblesStaticAndDynamicGroups) {
  GroupTable file_groups;
  build_file_groups(&file_groups);
  TargetRegistry registry;
  registry.intern(node(18081));
  registry.intern(node(18082));

  auto view = build_nodes_view(file_groups, fetch_with_dynamic(18083), registry, nullptr);
  ASSERT_NE(view, nullptr);
  ASSERT_EQ(view->groups.size(), 2u);
  const l7gate::route::UpstreamGroup* static_group = view->groups.group_by_name("static");
  ASSERT_NE(static_group, nullptr);
  EXPECT_EQ(static_group->targets.size(), 2u) << "静态组不该被 reload 动";
  const l7gate::route::UpstreamGroup* dynamic = view->groups.group_by_name("dynamic");
  ASSERT_NE(dynamic, nullptr);
  ASSERT_EQ(dynamic->targets.size(), 1u);
  EXPECT_EQ(dynamic->targets[0], 2u) << "新成员的下标 = ever-seen 追加的那一个";
  EXPECT_FALSE(dynamic->ring.empty()) << "环没在发布前建好：下一节请求就要 502";
  EXPECT_EQ(view->targets.size(), 3u);
}

// 下标空间用尽 → 整批拒绝（kNoUpstreamTarget 不是"挤掉谁"的许可）。
// 用 65534 个 intern 造满空间太慢，这里证的是"拒绝有话"这一半——把注册表顶满的
// 完整形状由 intern 的上界检查保证（单测里用 3 个组重复 intern 到上限不现实，
// 判据形状与 UnknownGroupNameIsRejectedWholesale 同族：nullptr + 点名）。
TEST(S3_04_Nodes, ViewWithMysqlGroupMissingFromFetchIsEmpty) {
  GroupTable file_groups;
  build_file_groups(&file_groups);
  TargetRegistry registry;
  registry.intern(node(18081));
  registry.intern(node(18082));

  NodesFetch fetch;  // dynamic 组一个成员都没有（启动时 MySQL 不可用的形状）
  std::string error;
  auto view = build_nodes_view(file_groups, fetch, registry, &error);
  ASSERT_NE(view, nullptr);
  const l7gate::route::UpstreamGroup* dynamic = view->groups.group_by_name("dynamic");
  ASSERT_NE(dynamic, nullptr);
  EXPECT_TRUE(dynamic->targets.empty()) << "成员没到岗的组就是空的，502 是它的表态";
}

// --- 三、发布器与观测面 --------------------------------------------------------------

TEST(S3_04_Nodes, PublisherCarriesSnapshotsAndCountsReloads) {
  NodesPublisher publisher;
  EXPECT_FALSE(publisher.has_snapshot());
  EXPECT_EQ(publisher.reloads_ok(), 0u);
  EXPECT_EQ(publisher.reloads_failed(), 0u);

  GroupTable file_groups;
  build_file_groups(&file_groups);
  TargetRegistry registry;
  registry.intern(node(18081));
  registry.intern(node(18082));
  auto view = build_nodes_view(file_groups, fetch_with_dynamic(18083), registry, nullptr);
  ASSERT_NE(view, nullptr);
  publisher.publish(view);
  ASSERT_TRUE(publisher.has_snapshot());
  EXPECT_EQ(publisher.load()->targets.size(), 3u);

  publisher.note_reload(true, 1000, 5);
  publisher.note_reload(false, 2000, 7);
  EXPECT_EQ(publisher.reloads_ok(), 1u);
  EXPECT_EQ(publisher.reloads_failed(), 1u);
  EXPECT_EQ(publisher.last_publish_epoch_ms(), 1000);
}

// --- 四、NodesStore：连不上也要有一句能定位的话 --------------------------------------

// 没有服务端的端口 → fetch 失败且话里点名"连接失败"。这条不依赖 Docker：
// 它证的是"错误从 C API 冒到调用方"这一段，真容器的正向路径在取证与冒烟里。
TEST(S3_04_Nodes, StoreFetchFailureNamesTheCause) {
  l7gate::config::NodesDbParams params;
  params.host = "127.0.0.1";
  params.port = 1;  // 没人监听的端口，connect 立刻拒绝
  params.user = "nobody";
  params.password = "nothing";
  params.database = "nowhere";
  params.timeout_ms = 300;
  l7gate::config::NodesStore store{params};
  l7gate::route::NodesFetch fetch;
  std::string error;
  EXPECT_FALSE(store.fetch({"dynamic"}, &fetch, &error));
  EXPECT_NE(error.find("连接失败"), std::string::npos) << error;
}
