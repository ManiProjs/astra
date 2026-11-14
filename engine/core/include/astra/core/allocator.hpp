#pragma once

#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace astra::core {

class Allocator {
  public:
    virtual ~Allocator() = default;

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    [[nodiscard]] virtual void* allocate(std::size_t size, std::size_t alignment) = 0;
    virtual void deallocate(void* memory, std::size_t size, std::size_t alignment) noexcept = 0;

  protected:
    Allocator() = default;
};

class DefaultAllocator final : public Allocator {
  public:
    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) override;
    void deallocate(void* memory, std::size_t size, std::size_t alignment) noexcept override;
};

struct AllocationStats final {
    std::size_t bytes_in_use{};
    std::size_t peak_bytes_in_use{};
    std::size_t live_allocations{};
    std::size_t total_allocations{};
};

// The upstream allocator must outlive this tracker and all its live allocations.
class TrackingAllocator final : public Allocator {
  public:
    explicit TrackingAllocator(Allocator& upstream) noexcept;

    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) override;
    void deallocate(void* memory, std::size_t size, std::size_t alignment) noexcept override;
    [[nodiscard]] AllocationStats stats() const noexcept;

  private:
    struct Allocation final {
        std::size_t size;
        std::size_t alignment;
    };

    Allocator& upstream_;
    mutable std::mutex mutex_;
    std::unordered_map<void*, Allocation> allocations_;
    AllocationStats stats_;
};

// The allocator must outlive every UniqueAllocation created from it.
class UniqueAllocation final {
  public:
    UniqueAllocation() noexcept = default;
    UniqueAllocation(Allocator& allocator, std::size_t size,
                     std::size_t alignment = alignof(std::max_align_t));
    ~UniqueAllocation();

    UniqueAllocation(const UniqueAllocation&) = delete;
    UniqueAllocation& operator=(const UniqueAllocation&) = delete;
    UniqueAllocation(UniqueAllocation&& other) noexcept;
    UniqueAllocation& operator=(UniqueAllocation&& other) noexcept;

    [[nodiscard]] void* get() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t alignment() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    void reset() noexcept;

  private:
    Allocator* allocator_ = nullptr;
    void* memory_ = nullptr;
    std::size_t size_ = 0;
    std::size_t alignment_ = 0;
};

} // namespace astra::core
