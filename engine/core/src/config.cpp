#include <astra/core/config.hpp>

#include <mutex>
#include <stdexcept>
#include <utility>

namespace astra::core {

void ConfigStore::set(std::string key, ConfigValue value) {
    if (key.empty()) {
        throw std::invalid_argument("configuration key must not be empty");
    }
    const std::unique_lock lock{mutex_};
    values_.insert_or_assign(std::move(key), std::move(value));
}

bool ConfigStore::set_default(std::string key, ConfigValue value) {
    if (key.empty()) {
        throw std::invalid_argument("configuration key must not be empty");
    }
    const std::unique_lock lock{mutex_};
    return values_.try_emplace(std::move(key), std::move(value)).second;
}

bool ConfigStore::erase(std::string_view key) {
    const std::unique_lock lock{mutex_};
    const auto entry = values_.find(key);
    if (entry == values_.end()) {
        return false;
    }
    values_.erase(entry);
    return true;
}

bool ConfigStore::contains(std::string_view key) const {
    const std::shared_lock lock{mutex_};
    return values_.contains(key);
}

std::vector<std::string> ConfigStore::keys() const {
    const std::shared_lock lock{mutex_};
    std::vector<std::string> result;
    result.reserve(values_.size());
    for (const auto& [key, value] : values_) {
        (void)value;
        result.push_back(key);
    }
    return result;
}

} // namespace astra::core
