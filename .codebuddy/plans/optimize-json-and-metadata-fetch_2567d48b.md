---
name: optimize-json-and-metadata-fetch
overview: 首轮优化：消除大 JSON 对象解析的 O(n²) 退化，修复并发抓取元数据时的临时文件冲突，并补充离线回归测试，保持现有下载与配置语义不变。
todos:
  - id: json-parser-fast-path
    content: 改造 mini_json 对象解析：小对象线性查重，大对象哈希索引，保持重复 key 后值覆盖
    status: completed
  - id: metadata-temp-unique
    content: 抽取 metaTempPath 并接入 fetchText，使用原子序号保证并发临时路径唯一
    status: completed
  - id: regression-tests
    content: 在 core_test 增加 2 万 key、重复 key 和 1000 个临时路径唯一性测试
    status: completed
    dependencies:
      - json-parser-fast-path
      - metadata-temp-unique
  - id: validate-build
    content: 运行 make、make check，并在 Qt6 可用时执行离屏冒烟测试
    status: completed
    dependencies:
      - regression-tests
---

## 用户需求
对现有 Minecraft 26.3 Linux 启动器做首轮代码优化，聚焦两处高收益、低风险问题，并补充离线回归测试：

- 消除大 JSON 对象解析的 O(n²) 退化，提升 asset index 等大文件的解析速度。
- 修复并发抓取元数据时临时文件撞名导致的偶发解析失败。
- 不改变界面、下载目录结构、镜像规则、SHA1 校验语义和配置格式。

## 产品概述
保持现有功能与界面完全不变，仅优化内部解析与下载元数据缓存逻辑，使大型版本/资源清单解析更快、并发下载更稳定。

## 核心功能
- 大对象 JSON 解析从 O(n²) 降为平均 O(n)，小对象保持低开销。
- 重复 key 继续遵循“后值覆盖”语义，JSON 读写 API 不变。
- 同一进程内并发抓取同一 URL 时，临时文件路径唯一，不再互相覆盖。
- 新增离线回归测试：2 万 key 对象解析、重复 key 语义、1000 个临时路径唯一性。
- 无视觉变化，无新增第三方依赖。


## 技术栈
- 语言与标准：C++23，保持 header-only。
- 现有框架：Qt6 Widgets 启动器，核心逻辑位于 `include/launcher_core.hpp`。
- 下载后端：继续使用系统 `curl` CLI，不引入 libcurl。
- 构建与测试：沿用 `Makefile`，`make check` 执行 `build/core-test` 与 `tests/downloader_test.sh`。
- 新增依赖：仅 C++ 标准库 `<unordered_map>`、`<atomic>`、`<chrono>`、`<set>`，不新增第三方库。

## 实现方案

### 1. JSON 大对象解析优化
文件：`/home/bus/Documents/deepseek-harness/workspace for github/include/mini_json.hpp`

现状：`JValue` 的对象使用 `std::vector<std::pair<std::string, JValue>>` 存储，`JValue::set()` 每次线性扫描已有 key；`Parser::object()` 对每个 key 调用 `set()`。asset index 的 `objects` 可能有数万 key，导致 O(n²)。

方案：
- 在 `Parser::object()` 内增加局部 `store` 逻辑，不修改 `JValue::set()` 的对外语义。
- 对象 key 数不超过 16 时，继续线性查重并追加，保持小对象低开销。
- 超过 16 个 key 时，惰性构建 `std::unordered_map<std::string, size_t>` 索引，后续插入/覆盖为平均 O(1)。
- 重复 key 仍更新原位置的值，保持“后值覆盖”，不产生重复条目。
- `index.reserve(out.o.size() * 2)` 降低 rehash 次数。
- 解析完成后仍使用现有 `vector` 存储，`find/array/object/str/number/boolean` 和 `jsonDump` 行为不变。
- 时间/空间复杂度：平均 O(n) 解析，额外 O(n) 索引内存；小对象无额外索引开销。

### 2. 元数据临时文件唯一化
文件：`/home/bus/Documents/deepseek-harness/workspace for github/include/download.hpp`

现状：`fetchText()` 的临时文件名由 `pid + std::hash(url)` 组成，同进程多线程抓同一 URL 会写同一个 `.tmp`。

方案：
- 新增命名空间级 `inline std::atomic<uint64_t>` 递增序号。
- 抽取 `metaTempPath(cacheDir, url)`，文件名包含 `pid + 原子序号 + URL hash`。
- `fetchText()` 改用该函数生成临时路径。
- 保持失败时删除临时文件、成功读取后删除、`.part` 下载逻辑和函数签名不变。
- 原子序号使用 `memory_order_relaxed`，开销可忽略；跨进程仍由 pid 区分。
- 该 helper 可直接被测试调用，无需启动 curl 或 mock 服务。

### 3. 离线回归测试
文件：`/home/bus/Documents/deepseek-harness/workspace for github/tests/core_test.cpp`

- 构造约 2 万 key 的嵌套对象 `{"objects":{"k0":{"hash":"h0"},...}}`，验证解析成功且 `objects` 数量正确。
- 记录解析耗时并设置宽松上限（例如 5000ms），用于防止 O(n²) 回归，避免机器性能导致误报。
- 验证重复 key：`{"a":1,"a":2,"b":3}` 解析后 `a == 2` 且对象只有 2 个 key。
- 连续生成 1000 个 `mc263::dl::metaTempPath(...)`，断言全部唯一。
- 测试保持离线，不依赖网络、Qt 或第三方库。

### 4. 性能与可靠性
- 2 万 key 对象修复后预计为毫秒级；旧实现为平方级比较，文件越大差距越明显。
- 小对象仍走线性路径，避免为每个小对象分配哈希表。
- 临时文件唯一化只增加一次原子自增，不引入锁竞争。
- JSON 解析错误、临时文件清理、下载失败语义均保持不变。

## 实施注意
- 不修改 `JValue::set()` 的签名和行为，避免影响配置生成、加载器安装等调用点。
- 哈希索引只存在于解析器局部，不进入 `JValue`，避免每个 JSON 值都携带额外容器。
- 重复 key 的“后值覆盖”必须保留，测试需覆盖。
- 计时阈值仅作为防退化保护，不作为严格基准；真实性能可用 `make` 后手动对比。
- 本轮不处理 Qt 回收站重复扫描、搜索建议线程治理、Mod 更新并发、CRC 表线程安全等 P1 项。

## 架构设计
本次改动局限在三个文件，不改变系统分层：

- JSON 解析层：`mini_json.hpp` 内部优化对象构建路径，外部 API 不变。
- 下载核心层：`download.hpp` 增加临时路径生成 helper，`fetchText()` 调用点最小改动。
- 测试层：`core_test.cpp` 增加针对解析性能、重复 key 和并发路径唯一性的离线测试。

数据流保持为：
`downloadAll()` 读取 asset index → `jsonParse()` → `Parser::object()` 构建对象 → 下载任务生成；优化只发生在对象构建阶段。

## 目录结构
```text
/home/bus/Documents/deepseek-harness/workspace for github/
├── include/
│   ├── mini_json.hpp    # [MODIFY] 在 Parser::object 中实现小对象线性查重 + 大对象哈希索引；
│   │                    # 保持 JValue::set 与 find/jsonDump 行为不变，新增 <unordered_map>。
│   └── download.hpp     # [MODIFY] 新增 metaTempPath(cacheDir, url) 与原子序号；
│                        # fetchText 改用唯一临时路径，保留清理和 .part 逻辑。
└── tests/
    └── core_test.cpp    # [MODIFY] 增加 2 万 key 解析、重复 key、1000 个临时路径唯一性测试；
                         # 补充 <chrono>/<set> 等标准库头文件。
```

## 关键代码结构
```cpp
// include/download.hpp
inline fs::path metaTempPath(const fs::path& cacheDir, const std::string& url);
```

该接口仅用于生成唯一元数据临时路径，不改变 `fetchText()` 的外部契约。

