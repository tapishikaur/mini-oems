#pragma once
// Names used on the wire (FIX Symbol, Account) and the small integer IDs used
// inside. IDs are dense, 0, 1, 2..., so they can index arrays directly.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace oems {

// Never assigned to a name: used for a name the directory doesn't know.
inline constexpr std::uint32_t kNoId = std::numeric_limits<std::uint32_t>::max();

class Directory {
public:
  // Returns the new ID. Throws std::invalid_argument if the name is already present.
  std::uint32_t add(std::string name) {
    if (ids_.contains(name)) throw std::invalid_argument("duplicate name: " + name);
    const auto id = static_cast<std::uint32_t>(names_.size());
    ids_.emplace(name, id);
    names_.push_back(std::move(name));
    return id;
  }

  std::optional<std::uint32_t> find(std::string_view name) const {
    const auto found = ids_.find(name);
    if (found == ids_.end()) return std::nullopt;
    return found->second;
  }

  bool contains(std::uint32_t id) const { return id < names_.size(); }
  const std::string& name(std::uint32_t id) const { return names_.at(id); }
  std::size_t size() const { return names_.size(); }

private:
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
  };
  std::vector<std::string> names_;
  std::unordered_map<std::string, std::uint32_t, StringHash, std::equal_to<>> ids_;
};

} // namespace oems
