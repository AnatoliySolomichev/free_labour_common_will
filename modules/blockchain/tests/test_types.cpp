#include "blockchain/types.h"
#include <gtest/gtest.h>

using namespace blockchain;

TEST(Hash, ZeroIsAllZeros) {
    auto h = Hash::zero();
    for (auto b : h.bytes) {
        EXPECT_EQ(b, 0);
    }
}

TEST(Hash, Equality) {
    Hash a, b;
    a.bytes[0] = 1;
    EXPECT_NE(a, b);
    b.bytes[0] = 1;
    EXPECT_EQ(a, b);
}

TEST(Signature, NullIsAllZeros) {
    auto s = Signature::null();
    for (auto b : s.bytes) {
        EXPECT_EQ(b, 0);
    }
}

TEST(NodeDepth, Root) {
    EXPECT_EQ(node_depth(0), 0u);
}

TEST(NodeDepth, Level1) {
    EXPECT_EQ(node_depth(1), 1u);
    EXPECT_EQ(node_depth(2), 1u);
}

TEST(NodeDepth, Level2) {
    EXPECT_EQ(node_depth(3), 2u);
    EXPECT_EQ(node_depth(4), 2u);
    EXPECT_EQ(node_depth(5), 2u);
    EXPECT_EQ(node_depth(6), 2u);
}

TEST(NodeDepth, MaxUint32) {
    // 2^32 - 1 is at depth 32 (first leaf at that level)
    EXPECT_EQ(node_depth(0xFFFFFFFFu), 32u);
}

TEST(IsLeafNode, Leaf) {
    EXPECT_TRUE(is_leaf_node(0xFFFFFFFFu));
}

TEST(IsLeafNode, NonLeaf) {
    EXPECT_FALSE(is_leaf_node(0));
    EXPECT_FALSE(is_leaf_node(1));
}

TEST(PathIndices, Root) {
    auto p = path_indices(0);
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0], 0u);
}

TEST(PathIndices, Level1Left) {
    auto p = path_indices(1);
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p[0], 0u);
    EXPECT_EQ(p[1], 1u);
}

TEST(PathIndices, Level2) {
    // node 3 is left child of node 1, which is left child of root 0
    auto p = path_indices(3);
    ASSERT_EQ(p.size(), 3u);
    EXPECT_EQ(p[0], 0u);
    EXPECT_EQ(p[1], 1u);
    EXPECT_EQ(p[2], 3u);
}

TEST(PathIndices, Level2Right) {
    // node 5 is left child of node 2 (right child of root)
    auto p = path_indices(5);
    ASSERT_EQ(p.size(), 3u);
    EXPECT_EQ(p[0], 0u);
    EXPECT_EQ(p[1], 2u);
    EXPECT_EQ(p[2], 5u);
}

TEST(NodeStruct, IsLeftChild) {
    Node n;
    n.index = 1;
    EXPECT_TRUE(n.is_left_child());
    n.index = 2;
    EXPECT_FALSE(n.is_left_child());
    n.index = 3;
    EXPECT_TRUE(n.is_left_child());
    n.index = 4;
    EXPECT_FALSE(n.is_left_child());
}

TEST(NodeStruct, ParentIndex) {
    Node n;
    n.index = 3;
    EXPECT_EQ(n.parent_index(), 1u);
    n.index = 1;
    EXPECT_EQ(n.parent_index(), 0u);
}
