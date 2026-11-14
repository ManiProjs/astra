#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace astra::core {

using ConfigValue = std::variant<bool, std::int64_t, double, std::string>;

class ConfigStore final {
  public:
    ConfigStore() = default;
    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    void set(std::string key, ConfigValue value);
    [[nodiscard]] bool set_default(std::string key, ConfigValue value);
    [[nodiscard]] bool erase(std::string_view key);
    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] std::vector<std::string> keys() const;

    template <typename T> [[nodiscard]] std::optional<T> get(std::string_view key) const {
        std::shared_lock lock{mutex_};
        const auto entry = values_.find(key);
        if (entry == values_.end()) {
            return std::nullopt;
        }
        if (const auto* value = std::get_if<T>(&entry->second)) {
            return *value;
        }
        return std::nullopt;
    }

    template <typename T> [[nodiscard]] T get_or(std::string_view key, T fallback) const {
        if (auto value = get<T>(key)) {
            return std::move(*value);
        }
        return fallback;
    }

  private:
    mutable std::shared_mutex mutex_;
    std::map<std::string, ConfigValue, std::less<>> values_;
};

} // namespace astra::core
