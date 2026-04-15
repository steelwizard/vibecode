#include "bst.h"

namespace algdat {

BinarySearchTree::BinarySearchTree() : root_(nullptr), size_(0) {}

BinarySearchTree::BinarySearchTree(const BinarySearchTree& other)
    : root_(clone_subtree(other.root_)), size_(other.size_) {}

BinarySearchTree::BinarySearchTree(BinarySearchTree&& other) noexcept
    : root_(other.root_), size_(other.size_) {
    other.root_ = nullptr;
    other.size_ = 0;
}

BinarySearchTree::~BinarySearchTree() {
    clear();
}

BinarySearchTree& BinarySearchTree::operator=(const BinarySearchTree& other) {
    if (this != &other) {
        Node* new_root = clone_subtree(other.root_);
        delete_subtree(root_);
        root_ = new_root;
        size_ = other.size_;
    }
    return *this;
}

BinarySearchTree& BinarySearchTree::operator=(BinarySearchTree&& other) noexcept {
    if (this != &other) {
        delete_subtree(root_);
        root_ = other.root_;
        size_ = other.size_;
        other.root_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

bool BinarySearchTree::insert(int value) {
    Node** current = &root_;
    while (*current != nullptr) {
        if (value == (*current)->value) {
            return false;
        }
        if (value < (*current)->value) {
            current = &((*current)->left);
        } else {
            current = &((*current)->right);
        }
    }

    *current = new Node(value);
    ++size_;
    return true;
}

bool BinarySearchTree::contains(int value) const {
    return contains_node(root_, value);
}

bool BinarySearchTree::remove(int value) {
    if (!remove_node(root_, value)) {
        return false;
    }
    --size_;
    return true;
}

bool BinarySearchTree::empty() const {
    return size_ == 0;
}

std::size_t BinarySearchTree::size() const {
    return size_;
}

void BinarySearchTree::clear() {
    delete_subtree(root_);
    root_ = nullptr;
    size_ = 0;
}

BinarySearchTree::Node* BinarySearchTree::clone_subtree(const Node* node) {
    if (node == nullptr) {
        return nullptr;
    }

    Node* copy = new Node(node->value);
    copy->left = clone_subtree(node->left);
    copy->right = clone_subtree(node->right);
    return copy;
}

void BinarySearchTree::delete_subtree(Node* node) {
    if (node == nullptr) {
        return;
    }
    delete_subtree(node->left);
    delete_subtree(node->right);
    delete node;
}

bool BinarySearchTree::contains_node(const Node* node, int value) {
    const Node* current = node;
    while (current != nullptr) {
        if (value == current->value) {
            return true;
        }
        if (value < current->value) {
            current = current->left;
        } else {
            current = current->right;
        }
    }
    return false;
}

bool BinarySearchTree::remove_node(Node*& node, int value) {
    if (node == nullptr) {
        return false;
    }

    if (value < node->value) {
        return remove_node(node->left, value);
    }
    if (value > node->value) {
        return remove_node(node->right, value);
    }

    if (node->left == nullptr && node->right == nullptr) {
        delete node;
        node = nullptr;
        return true;
    }

    if (node->left == nullptr) {
        Node* old = node;
        node = node->right;
        delete old;
        return true;
    }

    if (node->right == nullptr) {
        Node* old = node;
        node = node->left;
        delete old;
        return true;
    }

    Node* successor_parent = node;
    Node* successor = node->right;
    while (successor->left != nullptr) {
        successor_parent = successor;
        successor = successor->left;
    }

    node->value = successor->value;
    if (successor_parent == node) {
        successor_parent->right = successor->right;
    } else {
        successor_parent->left = successor->right;
    }
    delete successor;
    return true;
}

}  // namespace algdat
