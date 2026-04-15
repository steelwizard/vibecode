#include "bst.h"

#include <cassert>
#include <iostream>
#include <utility>

using algdat::BinarySearchTree;

static void test_insert_and_contains() {
    BinarySearchTree tree;

    assert(tree.insert(10));
    assert(tree.insert(5));
    assert(tree.insert(15));
    assert(!tree.insert(10));

    assert(tree.size() == 3);
    assert(tree.contains(10));
    assert(tree.contains(5));
    assert(tree.contains(15));
    assert(!tree.contains(99));
}

static void test_remove_leaf() {
    BinarySearchTree tree;
    tree.insert(10);
    tree.insert(5);
    tree.insert(15);

    assert(tree.remove(5));
    assert(!tree.contains(5));
    assert(tree.size() == 2);
}

static void test_remove_one_child() {
    BinarySearchTree tree;
    tree.insert(10);
    tree.insert(5);
    tree.insert(2);

    assert(tree.remove(5));
    assert(!tree.contains(5));
    assert(tree.contains(2));
    assert(tree.size() == 2);
}

static void test_remove_two_children() {
    BinarySearchTree tree;
    tree.insert(10);
    tree.insert(5);
    tree.insert(20);
    tree.insert(15);
    tree.insert(30);

    assert(tree.remove(20));
    assert(!tree.contains(20));
    assert(tree.contains(15));
    assert(tree.contains(30));
    assert(tree.size() == 4);
}

static void test_copy_and_move() {
    BinarySearchTree original;
    original.insert(8);
    original.insert(3);
    original.insert(12);

    BinarySearchTree copied(original);
    assert(copied.size() == 3);
    assert(copied.contains(8));
    assert(copied.contains(3));
    assert(copied.contains(12));

    BinarySearchTree assigned;
    assigned = original;
    assert(assigned.size() == 3);
    assert(assigned.contains(8));

    BinarySearchTree moved(std::move(original));
    assert(moved.size() == 3);
    assert(moved.contains(3));
    assert(original.empty());
}

static void test_clear_and_missing_remove() {
    BinarySearchTree tree;
    tree.insert(42);
    tree.insert(11);

    assert(!tree.remove(99));
    assert(tree.size() == 2);

    tree.clear();
    assert(tree.empty());
    assert(tree.size() == 0);
}

int main() {
    test_insert_and_contains();
    test_remove_leaf();
    test_remove_one_child();
    test_remove_two_children();
    test_copy_and_move();
    test_clear_and_missing_remove();

    std::cout << "All BinarySearchTree tests passed.\n";
    return 0;
}
