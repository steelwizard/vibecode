#include "linked_list.h"

#include <stdexcept>

namespace algdat {

LinkedList::LinkedList() : head_(nullptr), tail_(nullptr), size_(0) {}

LinkedList::LinkedList(const LinkedList& other) : head_(nullptr), tail_(nullptr), size_(0) {
    copy_from(other);
}

LinkedList::LinkedList(LinkedList&& other) noexcept
    : head_(other.head_), tail_(other.tail_), size_(other.size_) {
    other.head_ = nullptr;
    other.tail_ = nullptr;
    other.size_ = 0;
}

LinkedList::~LinkedList() {
    clear();
}

LinkedList& LinkedList::operator=(const LinkedList& other) {
    if (this != &other) {
        clear();
        copy_from(other);
    }
    return *this;
}

LinkedList& LinkedList::operator=(LinkedList&& other) noexcept {
    if (this != &other) {
        clear();
        head_ = other.head_;
        tail_ = other.tail_;
        size_ = other.size_;

        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void LinkedList::push_front(int value) {
    Node* new_node = new Node(value);
    new_node->next = head_;
    head_ = new_node;
    if (tail_ == nullptr) {
        tail_ = new_node;
    }
    ++size_;
}

void LinkedList::push_back(int value) {
    Node* new_node = new Node(value);
    if (tail_ == nullptr) {
        head_ = new_node;
        tail_ = new_node;
    } else {
        tail_->next = new_node;
        tail_ = new_node;
    }
    ++size_;
}

bool LinkedList::insert(std::size_t index, int value) {
    if (index > size_) {
        return false;
    }
    if (index == 0) {
        push_front(value);
        return true;
    }
    if (index == size_) {
        push_back(value);
        return true;
    }

    Node* previous = node_at(index - 1);
    Node* new_node = new Node(value);
    new_node->next = previous->next;
    previous->next = new_node;
    ++size_;
    return true;
}

bool LinkedList::remove_at(std::size_t index) {
    if (index >= size_) {
        return false;
    }

    Node* to_delete = nullptr;
    if (index == 0) {
        to_delete = head_;
        head_ = head_->next;
        if (size_ == 1) {
            tail_ = nullptr;
        }
    } else {
        Node* previous = node_at(index - 1);
        to_delete = previous->next;
        previous->next = to_delete->next;
        if (to_delete == tail_) {
            tail_ = previous;
        }
    }

    delete to_delete;
    --size_;
    return true;
}

int& LinkedList::at(std::size_t index) {
    if (index >= size_) {
        throw std::out_of_range("LinkedList::at index out of range");
    }
    return node_at(index)->value;
}

const int& LinkedList::at(std::size_t index) const {
    if (index >= size_) {
        throw std::out_of_range("LinkedList::at index out of range");
    }
    return node_at(index)->value;
}

bool LinkedList::empty() const {
    return size_ == 0;
}

std::size_t LinkedList::size() const {
    return size_;
}

void LinkedList::clear() {
    Node* current = head_;
    while (current != nullptr) {
        Node* next = current->next;
        delete current;
        current = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    size_ = 0;
}

LinkedList::Node* LinkedList::node_at(std::size_t index) const {
    Node* current = head_;
    for (std::size_t i = 0; i < index; ++i) {
        current = current->next;
    }
    return current;
}

void LinkedList::copy_from(const LinkedList& other) {
    Node* current = other.head_;
    while (current != nullptr) {
        push_back(current->value);
        current = current->next;
    }
}

}  // namespace algdat
