# wide-tree

不可变快照上的超宽树可见行投影。首版只回答一件事：一个节点可以有一百万个孩子，展开、跳转、折叠后再展开，仍然快，而且段数组不会碎掉。

## 首版冻结

- 快照不可变，子节点顺序固定，行高固定。
- 折叠只清掉该节点自己的展开偏好，后代偏好保留。
- 排序、过滤、异步分页不进核心。需要时由调用方重建，或直接接收 `ChangeKind::Reset`。
- 不提供语义含糊的 `expand(NodeId)`。可见行用 `expandAt` / `collapseAt`，隐藏节点用 `reveal`（只打开祖先，不展开目标自己）。

权威数据是展开位图。稀疏索引只为恢复加速。段数组只表示当前可见结构。`isExpanded(id)` 不是“它的孩子正在屏幕上”。

## 构建

```sh
make test
make asan
```

要求 C++20。`make test` 会跑验收：一百万个孩子、首屏、第 800000 行附近、展开带后代的节点、折叠父节点再展开并恢复、重复操作后段数稳定、旧 `RowRef` 被拒绝。

`make bench` 只打印微基准，不作为正确性门禁。它在同一套三种形状上采样 N、可达展开记录 K 和段数 S，用来看位图拷贝、段重建和稀疏索引分别在哪一档变贵。实测和阈值见 [docs/bench-baseline.md](docs/bench-baseline.md)。

## 最小调用

```cpp
#include "widetree/widetree.hpp"

widetree::TreeBuilder builder(nodeCount);
builder.setRoot(root);
builder.addChildRange(root, firstChild, fanout);
auto built = builder.build();

widetree::TreeView view;
view.reset(built.snapshot);

widetree::VisibleRow screen[100];
view.expandAt(*view.rowRef(0));
view.getVisibleRows(0, screen);
```

修改接口返回 `ProjectionChange`，核心里没有用户回调，避免展开尚未提交又重入折叠。展开图标所在的那一行在 `iconRows` 里，不能只根据后代的插入删除去刷新。

`RowRef` 同时绑着快照 id 和投影版本。重复展开、对叶子展开、纯绘制状态都不会让它失效。

## 明确不做

变高行、DAG、同一个 `NodeId` 出现在多处、`O(1)` 的 `isVisible`、修改路径上的零分配、池化索引、带权段树。段数组是否换成树，看段数 `S` 和修改频率，不看节点是否过百万。

契约细节见 [docs/v1-contract.md](docs/v1-contract.md)。

## 许可

MIT
