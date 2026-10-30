# ADR-20 · 引入 libmysqlclient 与节点表的 MySQL 来源（S3-04）

- 状态：已采纳（依赖引入与版本策略；**8.4 ↔ 8.0.36 的连通取证未完成——容器就位后实测，未取证前它不是既成事实**，契约 9.1 原话）
- 日期：2026-10-30
- 相关：ADR-06（MySQL 承担配置持久化与访问审计）、ADR-19（文件配置：组与路由是策略）、ADR-18（池按目标分桶）、ADR-15（per-worker 池）、ADR-14（跨线程接触面）、ADR-10（热重载 ≠ 热重启，S5）、契约 3.1 / 9.1 / 8.2 禁令三、TASKS「## S3」S3-04 行

## 一、依赖：libmysqlclient 8.4.11，不是别的

**我选 apt 源里的 `libmysqlclient-dev 8.4.11-0ubuntu0.26.04.1`，不引入任何封装层。**

- 26.04 的 apt 只有 8.4.11；契约 9.1 的偏离记录（8.0 无源、Oracle 侧停止支持）在环境取证时就写了，这里不重复论证，只认一个后果：**8.4 客户端 ↔ 8.0.36 服务端（`mysql:8.0.36` 镜像，锁定不动）的兼容性必须在真实连通里取证**——Oracle 矩阵说"新客户端连旧服务端"是允许方向，但矩阵不是证据。
- 不引第三方封装（Connector/C++、mariadb-connector-c、社会上的 ORM）：契约 1.3 的立场在网络层成立过一次（自研 epoll 而不是 asio），在存储客户端同样成立——我要的接口就是 C API 那二十个函数（`mysql_init/connect/query/store_result`），封装层买来的"省事"以多一个依赖、多一层错误语义为代价。错误处理我自己写：**每个返回码都判**，不"看起来连上了就继续"。
- 链接面：`mysql_config --libs` = `-lmysqlclient -lz -lzstd -lssl -lcrypto -lresolv -lm`。这批是 libmysqlclient 的传递依赖，随主库引入，不单独构成新的"第三方库"决策。
- **客户端侧取证已完成**（2026-10-30）：头存在、`g++ -std=c++20` 编译 + 链接 + 运行时 `mysql_get_client_info()` = 8.4.11（探针，只验库不建连接，已记 ENVIRONMENT 第 3 节）。

## 二、节点表的角色边界：MySQL 是"成员"的动态来源，文件仍是"策略"

S3 拆解已裁（2026-10-29）：S3-01～03 走文件配置、MySQL 节点表单排 S3-04，理由是"三件事的语义与配置来源无关"。这一条在 S3-04 落成边界就是：

- **文件（`--config`）继续拥有**：组名、策略（balance/hash_key/vnodes）、路由表、health 段、全部校验语义——一字不动。
- **MySQL 拥有**：某个组的**成员表**（哪些 地址:端口 在这个组里）。文件里的组可以声明 `"source": "mysql"`（**与 `targets` 互斥**，两者都给 = 启动失败——两个来源同时给一个组，"这个组有几个后端"就有两个答案，这是 ADR-19 对重复键表过态的那条立场）。
- 节点表**不做**审计（契约 3.1 说的访问审计是 S5+ 的格子，S3-04 的验收里没有它，禁令一挡住顺手做）。

为什么成员动态、策略静态：成员是**运维事实**（一台机器上线/下线），策略是**部署决定**（这组用哪种均衡）。混进同一处，改一台机器的 IP 就要动"策略文件"，而热重载策略是 S5 的格子——S3-04 只让事实动起来。

## 三、表结构（`l7gate_nodes`）

```sql
CREATE TABLE IF NOT EXISTS l7gate_nodes (
  group_name  VARCHAR(64)  NOT NULL,
  address     VARCHAR(45)  NOT NULL,
  port        SMALLINT UNSIGNED NOT NULL,
  health_path VARCHAR(128) NOT NULL DEFAULT '/',
  PRIMARY KEY (group_name, address, port)
);
```

- 主键即去重约束：同一组同一后端一行，插不进第二行——ADR-19 对组内重复成员的"拒绝"在数据库侧由主键背书。
- `health_path` 跟着行走（探活是后端的属性，ADR-19 追加四已定）；同一 `address:port` 出现在两个组、`health_path` 不同——**允许**，与文件不同：文件去重是因为同一台机器不该有两个"活着"的定义，但那是**同一份配置内部**的约束；两个组对同一台机器各配各的探活路径在 MySQL 形态下没有"同一份"可争（组的装配各自独立）。这条与文件侧的差异我刻意保留并写在这里。
- 校验沿用文件侧那张表：port 1..65535、health_path 非空且 `/` 开头、组名必须**已在文件里声明**（MySQL 里出现文件没有的组名 = 整批拒绝加载，保留上一份——不是"多加载一个组"，那等于让数据库偷偷扩配置面）。

## 四、reload 机制：查表在专用线程，发布是"整张表换一根指针"

这是本 ADR 最重的一条设计，先把三条硬约束摆出来：

1. **MySQL 查询是阻塞 IO，不进 worker 事件循环**——探活的先例（归 owner 循环）在这里不适用：探活是非阻塞 socket 上的几字节，reload 是毫秒级的阻塞往返。专用 reload 线程（`std::thread` 一条，周期 = `nodes_reload_ms`，缺省 5000，0 = 只在启动时读一次）。
2. **发布必须是原子快照**：worker 的 `LoadBalanceFilter` 读 `GroupTable`，读到一半换表等于读到半张环。形状与 ADR-10/S5 预告的同一家：`std::atomic<std::shared_ptr<const GroupTable>>` 的 `load`/`store`，新表在 reload 线程**装配完整**（含 `build_hash_ring`）才发布——装配期建环的纪律在新表上原样成立。filter 侧从"引用"改成"每请求 load 一根指针"（一次原子读，热路径可承受）。
3. **目标下标空间只增不减（ever-seen）**：新成员出现不能让 per-worker 的 `LbState`（配对账）、`upstream_pool_`（桶）、`HealthTable`（健康状态）当场 resize——那些数组归 worker 循环线程，运行期变长就是数据竞争。所以目标表只**追加**：删掉的成员保留槽位、从候选集剔除（等价于"永久摘除"但不占安全阀名额——它不是被摘的，是不在了）；新增成员追加到表尾、各 per-worker 面在下一次 swap 时按需扩容（扩容动作本身在 worker 循环的 tick 里做，不是在 reload 线程）。这条的验收形状就是 S3-04 验收句的"reload 生效耗时实测"：增删一个后端到路由表反映出来的秒数，两端用 MySQL 行变更时刻与网关自报计数钉。

代价照记：ever-seen 意味着长期运行 + 频繁换后端会让目标表单调变长（槽位是 O(成员历史) 不是 O(成员)）。每行只占 `Target` + 三个 vector 槽位，量级在 KB/千次变更，我不为它做"槽位复用"的聪明——那是没有证据的优化（契约 3.2）。

**reload 线程与 ADR-14 的关系**：跨线程流动的只有不可变的 `GroupTable` 快照指针与几次原子长度读取；连接对象、fd、per-worker 状态一概不流动。`tsan` 零 data race 照旧是可证伪命题。

## 五、降级路径（验收句原话的落点）

- **运行期 MySQL 挂了**：reload 线程的下一拍失败 → 保留当前已加载的表、计 `nodes_reload_failures` 计数、**不拒绝、不 500**——请求按上一份成员继续服务。恢复后第一次成功 reload 换上新表（`recoveries` 可断言）。
- **启动时 MySQL 不可用**：`source: mysql` 的组在启动时加载失败 → 该组空成员、网关照常启动（LB 对空组回 502 是既有语义），reload 恢复后补上成员。**不退出**——"按已加载的配置继续服务"从启动那一刻就成立。
- 查询超时给上限（`nodes_timeout_ms`，缺省 2000）：一个挂死的 mysqld 不能把 reload 线程变成僵尸。

## 六、验收形状（进 TASKS 的就是这几句）

- 首次连通：`8.4.11` 客户端 ↔ `mysql:8.0.36` 服务端真实跑通（connect + 建表 + 增删行 + 读回），结果进 ENVIRONMENT/BENCHMARK——取证前第 0 节那句"不算既成事实"不撤销。
- reload 生效耗时：MySQL 里 INSERT 一个成员（T0）→ 网关把请求发到它（T1），两端钉住报秒数；DELETE 后对称测"不再收请求"。
- 降级：mysqld stop 后业务零中断（错误响应数 = 0）、恢复后新成员到岗；全程 `requests_parsed` 不掺 reload 的查询。
- 阳性验证：`source` 与 `targets` 同给 → 启动拒绝；MySQL 中未知组名 → 整批拒载保留旧表；reload 线程掐掉后（`nodes_reload_ms=0`）成员变更永不生效。

## 追加（2026-10-30，S3-04 落地时）：第三节"跨组允许不同 health_path"被实现推翻，更正为拒绝

第三节说两个组对同一台机器各配各的探活路径"允许"——落地时这格立不住：**探活是每目标的
一条状态机，而 ever-seen 注册表按 `地址:端口` 认机器**（下标跨 reload 稳定是池桶/配对账/
健康表全部成立的地基），同一台机器在注册表里只有一行、一个 `health_path`（取第一次 intern
的定义）。所以文件侧"同一台机器两个 health_path = 拒绝"的老规矩在 MySQL 形态下原样成立：
`nodes_store` 与 `build_nodes_view` 双重拦截，同一端点带着不同 `health_path` 出现 = 整批拒载。
当时写"允许"的理由（"没有同一份配置可争"）忽略了探活机的粒度——**语义跟着机制走，
机制变了语义就得重新对**，这是这条更正的教训。

其余各节按落地原样成立，除了两处细化的补充：reload 生效粒度 = `--nodes-reload-ms`（B-015
实测 406/536 ms @ 500ms 间隔）；ever-seen 槽位不回收（O(成员历史) 的账记在 ARCHITECTURE 1.26）。
