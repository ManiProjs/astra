#include <astra/core/allocator.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace astra::core {
namespace {

void validate_allocation(std::size_t size, std::size_t alignment) {
    if (size == 0) {
        throw std::invalid_argument("allocation size must be greater than zero");
    }
    if (alignment < alignof(void*) || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument(
            "allocation alignment must be a power of two and at least alignof(void*)");
    }
}

} // namespace

void* DefaultAllocator::allocate(std::size_t size, std::size_t alignment) {
    validate_allocation(size, alignment);
    if (alignment > alignof(std::max_align_t)) {
        return ::operator new(size, std::align_val_t{alignment});
    }
    return ::operator new(size);
}

void DefaultAllocator::deallocate(void* memory, std::size_t, std::size_t alignment) noexcept {
    if (memory != nullptr) {
        if (alignment > alignof(std::max_align_t)) {
            ::operator delete(memory, std::align_val_t{alignment});
        } else {
            ::operator delete(memory);
        }
    }
}

TrackingAllocator::TrackingAllocator(Allocator& upstream) noexcept : upstream_(upstream) {}

void* TrackingAllocator::allocate(std::size_t size, std::size_t alignment) {
    validate_allocation(size, alignment);
    void* memory = upstream_.allocate(size, alignment);
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }

    bool inserted = false;
    try {
        const std::lock_guard lock{mutex_};
        if (size > std::numeric_limits<std::size_t>::max() - stats_.bytes_in_use ||
            stats_.total_allocations == std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("allocation tracking counters overflow");
        }
        const auto [entry, was_inserted] =
            allocations_.emplace(memory, Allocation{size, alignment});
        (void)entry;
        inserted = was_inserted;
        if (inserted) {
            stats_.bytes_in_use += size;
            stats_.peak_bytes_in_use = std::max(stats_.peak_bytes_in_use, stats_.bytes_in_use);
            ++stats_.live_allocations;
            ++stats_.total_allocations;
        }
    } catch (...) {
        upstream_.deallocate(memory, size, alignment);
        throw;
    }
    if (!inserted) {
        throw std::runtime_error("upstream allocator returned an already-live address");
    }
    return memory;
}

void TrackingAllocator::deallocate(void* memory, std::size_t, std::size_t) noexcept {
    if (memory == nullptr) {
        return;
    }

    Allocation allocation{};
    {
        const std::lock_guard lock{mutex_};
        const auto entry = allocations_.find(memory);
        if (entry == allocations_.end()) {
            return;
        }
        allocation = entry->second;
        allocations_.erase(entry);
        stats_.bytes_in_use -= allocation.size;
        --stats_.live_allocations;
    }
    upstream_.deallocate(memory, allocation.size, allocation.alignment);
}

AllocationStats TrackingAllocator::stats() const noexcept {
    const std::lock_guard lock{mutex_};
    return stats_;
}

UniqueAllocation::UniqueAllocation(Allocator& allocator, std::size_t size, std::size_t alignment)
    : allocator_(&allocator), size_(size), alignment_(alignment) {
    validate_allocation(size, alignment);
    memory_ = allocator.allocate(size, alignment);
    if (memory_ == nullptr) {
        allocator_ = nullptr;
        throw std::bad_alloc{};
    }
}

UniqueAllocation::~UniqueAllocation() {
    reset();
}

UniqueAllocation::UniqueAllocation(UniqueAllocation&& other) noexcept
    : allocator_(std::exchange(other.allocator_, nullptr)),
      memory_(std::exchange(other.memory_, nullptr)), size_(std::exchange(other.size_, 0)),
      alignment_(std::exchange(other.alignment_, 0)) {}

UniqueAllocation& UniqueAllocation::operator=(UniqueAllocation&& other) noexcept {
    if (this != &other) {
        reset();
        allocator_ = std::exchange(other.allocator_, nullptr);
        memory_ = std::exchange(other.memory_, nullptr);
        size_ = std::exchange(other.size_, 0);
        alignment_ = std::exchange(other.alignment_, 0);
    }
    return *this;
}

void* UniqueAllocation::get() const noexcept {
    return memory_;
}

std::size_t UniqueAllocation::size() const noexcept {
    return size_;
}

std::size_t UniqueAllocation::alignment() const noexcept {
    return alignment_;
}

UniqueAllocation::operator bool() const noexcept {
    return memory_ != nullptr;
}

void UniqueAllocation::reset() noexcept {
    if (allocator_ != nullptr && memory_ != nullptr) {
        allocator_->deallocate(memory_, size_, alignment_);
    }
    allocator_ = nullptr;
    memory_ = nullptr;
    size_ = 0;
    alignment_ = 0;
}

} // namespace astra::core
