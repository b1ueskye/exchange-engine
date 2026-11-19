# 从 Java 迁移到 C++ 时要重新检查的假设

| 主题 | Java 源码中的落点 | C++ 课程做法与验证 |
| --- | --- | --- |
| 对象和值 | `Order` 是堆对象的引用语义，Naive 的档和 ID 表可指向同一个订单。HotSpot 的物理布局还受 JVM、对象头和优化影响 | `RestingOrder` 按值嵌在容器节点中。`b = a` 通常表示赋值，含容器的值复制可能递归复制元素。Day 02 用别名与独立副本分别修改数量 |
| GC 与 RAII | 不可达对象由 GC 回收。Java 的文件仍需 `try-with-resources`。Direct 对象池减少热点分配 | 容器、锁、文件句柄、线程都有明确所有者与释放路径。析构顺序受作用域和成员声明次序约束。Day 03 在提前返回时验证资源释放 |
| 引用、指针与观察期 | 引用保持对象可达，但环槽复用会覆盖对象内容 | `T&` 是借用，`T*` 可空且所有权须从约定判断，`unique_ptr<T>` 显式独占。擦除后借用可能悬空。Day 10 用索引和容器失效规则检查撤单 |
| 集合 | Naive 用 `TreeMap`、`LinkedHashMap` 和原始键散列表。Direct 用基数树及侵入链 | 用 `map`、`list`、`unordered_map` 组合价格、FIFO 和 ID 访问。散列表的 rehash 影响其迭代器，链表节点的有效期由链表操作决定。Day 10 分别触发这两种变化 |
| 泛型和模板 | `IOrderBook` 的实现由工厂选择。普通 Java 泛型以擦除为主，原始数值常需专用集合 | 模板可直接实例化整数和值类型，编译器生成特定代码。`optional<T>` 内联管理值的存在状态。Day 04 用不同 `T` 读错误信息，Day 24 再比较运行期分派 |
| 接口、多态和 ABI | `IOrderBook.processCommand` 分派命令，订单簿实现可替换 | C++ 先写具体类型，第二种实现出现时以纯虚接口和虚析构通过 `unique_ptr<Book>` 管理。虚表是常见实现细节，运行期成本看调用分布与编译优化 |
| 异常和错误 | 风控、查单失败等返回 `CommandResultCode`，破坏内部假设的路径可抛异常 | 业务拒绝用 `Status` 和事件，算术帮助函数用可空结果，基础设施错误在边界转换。Day 17 检验失败时没有局部提交。析构和状态提交路径约定不抛出 |
| 整数语义 | Java `long` 运算有定义的截断环绕，`CoreArithmeticUtils` 直接乘加 | C++ 有符号溢出是未定义行为。Day 01 在乘法前检查上界，Day 06 扩展到带符号会计和尺度约束 |
| 并发和可见性 | Disruptor 发布和序列屏障提供阶段交接，Java `volatile` 与原子类有同步含义 | 用 mutex、条件变量建立先行发生关系，再学 `atomic` 的 release/acquire。C++ `volatile` 不能用于线程同步。Day 26 用有界 SPSC 实验槽位所有权 |
| 布局和热点 | Java 池化、原始键索引、复用环槽可减少装箱、分配和部分指针访问 | `vector<T>` 连续存放元素，`list<T>` 通常要跨节点间接访问。`sizeof` 只测对象本身并包括 padding，不覆盖外部分配。Day 21–24 在同负载下测多种布局 |
| 持久化 | Chronicle 写字段并由 LZ4 压缩，恢复构造器重建内部结构 | 使用显式版本、字节序和校验，忽略内存地址、填充和容器内部布局。Day 29 验证快照、纯重放及混合恢复的一致性 |

[Naive 档和索引](https://github.com/exchange-core/exchange-core/blob/2f8548749839e9095c8dc597e4b61521d259fa5d/src/main/java/exchange/core2/core/orderbook/OrderBookNaiveImpl.java#L34-L78) · [Java 金额公式](https://github.com/exchange-core/exchange-core/blob/2f8548749839e9095c8dc597e4b61521d259fa5d/src/main/java/exchange/core2/core/utils/CoreArithmeticUtils.java#L24-L43) · [C++ 对象生命周期条款](https://eel.is/c++draft/basic.life)

交易规则适合通过两边都能读懂的夹具转移。例如一笔现货买单在卖一价成交后，比较 maker ID、成交价量、买卖余额和手续费。C++ 的值拷贝是否开销大、池化是否有收益、原子队列是否改善尾延迟，要分开测量。跨语言比较还需固定输入分布、预热、编译配置、GC、线程绑定、计时边界和硬件。基准同时记录吞吐、p50/p99、在途量及内存，以便解释改善出现在哪个阶段。

[前往课程路线图](learning-roadmap.md)
