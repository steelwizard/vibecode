#ifndef ALGDAT_LINKED_LIST_H
#define ALGDAT_LINKED_LIST_H

#include <cstddef>

namespace algdat {

class LinkedList {
public:
    LinkedList();
    LinkedList(const LinkedList& other);
    LinkedList(LinkedList&& other) noexcept;
    ~LinkedList();

    LinkedList& operator=(const LinkedList& other);
    LinkedList& operator=(LinkedList&& other) noexcept;

    void push_front(int value);
    void push_back(int value);
    bool insert(std::size_t index, int value);
    bool remove_at(std::size_t index);

    int& at(std::size_t index);
    const int& at(std::size_t index) const;

    bool empty() const;
    std::size_t size() const;
    void clear();

private:
    struct Node {
        int value;
        Node* next;

        explicit Node(int v) : value(v), next(nullptr) {}
    };

    Node* head_;
    Node* tail_;
    std::size_t size_;

    Node* node_at(std::size_t index) const;
    void copy_from(const LinkedList& other);
};

}  // namespace algdat

#endif  // ALGDAT_LINKED_LIST_H
