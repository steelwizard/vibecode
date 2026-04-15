#ifndef ALGDAT_BST_H
#define ALGDAT_BST_H

#include <cstddef>

namespace algdat {

class BinarySearchTree {
public:
    BinarySearchTree();
    BinarySearchTree(const BinarySearchTree& other);
    BinarySearchTree(BinarySearchTree&& other) noexcept;
    ~BinarySearchTree();

    BinarySearchTree& operator=(const BinarySearchTree& other);
    BinarySearchTree& operator=(BinarySearchTree&& other) noexcept;

    bool insert(int value);
    bool contains(int value) const;
    bool remove(int value);

    bool empty() const;
    std::size_t size() const;
    void clear();

private:
    struct Node {
        int value;
        Node* left;
        Node* right;

        explicit Node(int v) : value(v), left(nullptr), right(nullptr) {}
    };

    Node* root_;
    std::size_t size_;

    static Node* clone_subtree(const Node* node);
    static void delete_subtree(Node* node);
    static bool contains_node(const Node* node, int value);
    static bool remove_node(Node*& node, int value);
};

}  // namespace algdat

#endif  // ALGDAT_BST_H
