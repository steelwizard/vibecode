#include "linked_list.h"

#include <cassert>
#include <iostream>
#include <stdexcept>

using algdat::LinkedList;

static void test_push_and_at() {
    LinkedList list;
    list.push_back(10);
    list.push_back(20);
    list.push_front(5);

    assert(list.size() == 3);
    assert(!list.empty());
    assert(list.at(0) == 5);
    assert(list.at(1) == 10);
    assert(list.at(2) == 20);
}

static void test_insert() {
    LinkedList list;
    list.push_back(1);
    list.push_back(3);

    assert(list.insert(1, 2));
    assert(list.at(0) == 1);
    assert(list.at(1) == 2);
    assert(list.at(2) == 3);
    assert(list.size() == 3);

    assert(!list.insert(10, 99));
}

static void test_remove() {
    LinkedList list;
    list.push_back(1);
    list.push_back(2);
    list.push_back(3);

    assert(list.remove_at(1));
    assert(list.size() == 2);
    assert(list.at(0) == 1);
    assert(list.at(1) == 3);

    assert(list.remove_at(0));
    assert(list.remove_at(0));
    assert(list.empty());
    assert(!list.remove_at(0));
}

static void test_copy_and_move() {
    LinkedList original;
    original.push_back(7);
    original.push_back(8);

    LinkedList copied(original);
    assert(copied.size() == 2);
    assert(copied.at(0) == 7);
    assert(copied.at(1) == 8);

    LinkedList assigned;
    assigned = original;
    assert(assigned.size() == 2);
    assert(assigned.at(0) == 7);
    assert(assigned.at(1) == 8);

    LinkedList moved(std::move(original));
    assert(moved.size() == 2);
    assert(moved.at(0) == 7);
    assert(moved.at(1) == 8);
    assert(original.empty());
}

static void test_out_of_range() {
    LinkedList list;
    bool threw = false;
    try {
        (void)list.at(0);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);
}

int main() {
    test_push_and_at();
    test_insert();
    test_remove();
    test_copy_and_move();
    test_out_of_range();

    std::cout << "All LinkedList tests passed.\n";
    return 0;
}
