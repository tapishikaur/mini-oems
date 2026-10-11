#include "oems/fix_settings.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace oems {

std::string fix44DictionaryPath() { return MINI_OEMS_FIX44_XML; }

FIX::SessionSettings loadSessionSettings(const std::string& path) {
  std::ifstream file(path);
  if (!file) throw std::runtime_error("can't open " + path);
  std::stringstream raw;
  raw << file.rdbuf();

  std::string text = raw.str();
  constexpr std::string_view placeholder = "${FIX44_XML}";
  const std::string dictionary = fix44DictionaryPath();
  for (std::size_t at = text.find(placeholder); at != std::string::npos;
       at = text.find(placeholder, at + dictionary.size())) {
    text.replace(at, placeholder.size(), dictionary);
  }
  std::istringstream settings(text);
  return FIX::SessionSettings(settings);
}

} // namespace oems
