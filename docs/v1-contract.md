# v1 契约

首版范围已经冻结。实现若与本文冲突，以本文的可观察行为为准。

## 放弃的复杂度

这些不在 v1 核心里，调用方不要假设它们存在：

- 排序、过滤、异步分页。边界只留 `ChildOrderPolicy::Fixed` 和 `ChangeKind::Reset`。
- 变高行。行号就是可见序号。
- 公开的 `expand(NodeId)`。
- DAG，或同一个 `NodeId` 表示多个树位置。`rowOf` 最多一个答案。
- 森林。构建器拒绝第二个根。展示根的子序列以后可以挂多个真根，v1 不打开这条路径。
- `isVisible` / `rowOf` 的 O(1)。v1 允许沿祖先行走，并扫描段数组。
- 修改路径零分配。零分配只约束查询：`rowCount`、`isExpanded`、`getVisibleRows` 写入调用方缓冲区。
- 把段数组换成树就自动得到 `O(log S)` 的展开、定位和恢复。v1 不升级这个结构。

## 展示根

内部有一个不占 `NodeId` 的展示根，永远展开，不能被选中、折叠或当作业务节点返回。

| 模式 | 展示根的子序列 | 首行深度 |
| --- | --- | --- |
| 显示真根 | `[真根]` | 0 |
| 隐藏真根 | 真根的直接孩子 | 0 |

隐藏真根时，不会把真根的 expanded 位强制设成 true。

## 三套状态

| 记录 | 角色 |
| --- | --- |
| 展开位图 | 展开偏好的权威 |
| 稀疏索引 | 每个父节点上，哪些直接孩子偏好展开，按 `rowInParent` 有序。只给真正有这种孩子的父节点建 span |
| 段数组 + 前缀和 | 当前可见结构 |

因此一个节点可以 `isExpanded == true` 且 `isVisible == false`：它的祖先折叠着，偏好还在。

稀疏索引是按 parent 有序的 span 表，加一段被这些 span 切分的 ordinal 数组。候选在改到某个 parent 之前与当前索引共享同一份载荷；第一次改 span 才复制这两块连续内存。根的展开和折叠不改索引，不付这笔拷贝。

曾经每个 parent 一个 `std::map` 节点。整份拷贝在大约两千个 parent 时越过 100 µs，大约一万到两万个 parent 时越过 1 ms，而段数组当时还远没到这个门槛。见 [bench-baseline.md](bench-baseline.md)。

## 恢复

只沿着实际展开的路径向下。父节点折叠时，不再访问它子孙的展开记录。

```text
emitExpandedChildren(parent, childDepth):
    begin = 0
    for each expanded direct child in order:
        emit parent children [begin, ordinal + 1)
        emitExpandedChildren(child, childDepth + 1)
        begin = ordinal + 1
    emit parent children [begin, childCount)
```

实现用显式栈。设本次可达的展开记录数是 K，生成段是 O(K + 段数)，不随兄弟数量增长。把段数组提交进视图仍然是 O(S)。两笔成本分开报。

一百万个孩子、只有两个展开时，段是常数个，而不是一百万个。

## 修改

1. 校验快照和 `RowRef`（版本，外加该行仍然是记录的 `NodeId`）。
2. 位图在副本上改。稀疏索引也在副本上改，但没被碰到的 parent 不复制。
3. 用副本生成候选段数组和前缀和。
4. 不再分配的提交阶段交换投影和状态，增加投影版本。
5. 返回 `ProjectionChange`。

候选重建自然满足合并不变量：一次修改结束后，相邻段不存在“同一父节点、同一深度、首尾相接”的可合并对。折叠不清除后代偏好。

`reveal(id)` 打开祖先路径，使 `id` 可见，不展开 `id` 自己。祖先一次改完，只重建一次投影。

重复展开、展开叶子、重复折叠：`NoChange`，不增加投影版本。

## 版本

| 版本 | 何时变化 | 影响 |
| --- | --- | --- |
| `SnapshotId` | 换底层快照 | `NodeId` 和借出的 `string_view` |
| `ProjectionRevision` | 可见行的身份、顺序或深度变化 | `RowRef` |

悬停、选中、焦点不属于本核心，不得为了它们增加投影版本。

## 变更通知

```cpp
struct ProjectionChange {
    std::uint64_t oldRevision, newRevision;
    RowCount oldRowCount, newRowCount;
    ChangeKind kind;          // NoChange, Splice, Reset
    RowSplice splice;         // Reset 时忽略
    std::vector<RowIndex> iconRows;
};
```

展开通常是 `{first: row + 1, removed: 0, inserted: 恢复出来的后代行数}`。折叠对偶。`iconRows` 是原先就可见、展开图标发生翻转的行。新插入的行自己带着图标，但父行必须另外刷新。

核心修改过程中没有用户回调。

## 构建器

输入按调用顺序成为该父节点的永久子顺序。流程是计数、前缀和、一次分配、再填入，不为每个节点建一个 `vector`。

发布前检查：`NodeId` 范围、单父、自环、环、恰好一个根、从根可达、孩子切片不越界、字符串范围、计数与偏移不溢出。

## 内存

不要用 `sizeof(NodeTopology)` 估计整体。

- `NodeTopology` 16 字节，`NodeInfo` 16 字节，每条边一个 `uint32` 孩子 id。
- 一百万节点、约一百万条边，快照常驻大约 34 MB，不含字符串池、容器容量和视图。
- 每个视图另有位图（约 N/8 字节）、稀疏索引、段数组和前缀和。
- 索引常驻是 span 表加 ordinal 数组。四万个 parent、每个一个 ordinal 时大约 640 KB，不是每个 parent 一次堆分配。
- 修改时候选投影与旧投影短暂并存。索引载荷未改时不在此列。段数组的 O(S) 成本仍然接受。

`TreeView::memory()` 分开报告快照、位图、索引、段数组。索引载荷是 ordinal 数组；另一项是 span 表的容量，加上 ordinal 数组还没用上的容量。

## 验收

1. 一个节点拥有 100 万个孩子。
2. 展开并读取首屏 100 行。
3. 跳到第 800000 行附近。
4. 展开其中几个带后代的节点。段必须是常数个，并符合 `[0, 11)`、后代、`[11, 800001)`、后代、`[800001, 1000000)` 的切分。
5. 折叠父节点。跨在展开点两侧的父段必须合并。
6. 再展开父节点，后代展开偏好恢复。
7. 重复折叠/展开，段数、展开数、索引父节点数稳定。
8. 旧 `RowRef`、对不上的 `NodeId`、过期快照 id 都被拒绝。

隐藏真根时，展示根的五段切分同样成立，且真根的 expanded 位保持 false。
