#include "ros2_adapter.hpp"

#include <ament_index_cpp/get_package_share_directory.hpp>

#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace robot_mcap::ros2 {
namespace {

constexpr const char* kSeparator =
    "================================================================================\n";

struct ParsedType {
  std::string package;
  std::string kind;  // msg, srv or action
  std::string name;
};

std::string trim(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

std::vector<std::string> split(std::string_view value, char separator) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find(separator, begin);
    if (end == std::string_view::npos) {
      parts.emplace_back(value.substr(begin));
      break;
    }
    parts.emplace_back(value.substr(begin, end - begin));
    begin = end + 1;
  }
  return parts;
}

bool parse_type(const std::string& value, ParsedType& parsed, std::string& error) {
  const auto parts = split(value, '/');
  if (parts.size() == 3 && !parts[0].empty() && !parts[2].empty()) {
    parsed = {parts[0], parts[1], parts[2]};
    if (parsed.kind != "msg" && parsed.kind != "srv" && parsed.kind != "action") {
      error = "unsupported ROS type kind: " + value;
      return false;
    }
    return true;
  }
  if (parts.size() == 2 && !parts[0].empty() && !parts[1].empty()) {
    parsed = {parts[0], "msg", parts[1]};
    return true;
  }
  error = "invalid ROS type name: " + value;
  return false;
}

bool is_builtin(std::string_view type) {
  static const std::set<std::string> kBuiltins{
      "bool",   "byte",    "char",    "float32", "float64", "int8",
      "uint8",  "int16",   "uint16",  "int32",   "uint32",  "int64",
      "uint64", "string",  "wstring", "time",    "duration"};
  return kBuiltins.find(std::string(type)) != kBuiltins.end();
}

std::string strip_array(std::string type) {
  const auto bracket = type.find('[');
  if (bracket != std::string::npos) {
    type.erase(bracket);
  }
  return type;
}

bool read_file(const std::filesystem::path& path, std::string& contents,
               std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot read " + path.string();
    return false;
  }
  contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  if (file.bad()) {
    error = "cannot read " + path.string();
    return false;
  }
  return true;
}

bool collect(const ParsedType& type, std::set<std::string>& seen, std::string& out,
             std::string& error) {
  const std::string key = type.package + "/" + type.kind + "/" + type.name;
  if (!seen.insert(key).second) {
    return true;
  }

  std::filesystem::path share;
  try {
    share = ament_index_cpp::get_package_share_directory(type.package);
  } catch (const std::exception& exception) {
    error = "package not installed: " + type.package + " (" + exception.what() + ")";
    return false;
  }

  auto msg_path = share / type.kind / (type.name + ".msg");
  std::string text;
  bool idl = false;
  if (!std::filesystem::exists(msg_path)) {
    auto idl_path = share / type.kind / (type.name + ".idl");
    if (!std::filesystem::exists(idl_path)) {
      error = "message definition not installed: " + key;
      return false;
    }
    msg_path = std::move(idl_path);
    idl = true;
  }
  if (!read_file(msg_path, text, error)) {
    return false;
  }
  if (out.empty()) {
    out = text;
  } else {
    out += kSeparator;
    out += text;
  }
  if (!out.empty() && out.back() != '\n') {
    out.push_back('\n');
  }
  if (idl) {
    // The installed .idl is used verbatim; dependency expansion is only implemented
    // for .msg definitions in v1.
    return true;
  }

  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim(line);
    if (line.empty() || line.find('=') != std::string::npos) {
      continue;
    }
    std::istringstream fields(line);
    std::string field_type;
    fields >> field_type;
    if (field_type.empty()) {
      continue;
    }
    field_type = strip_array(field_type);
    if (is_builtin(field_type)) {
      continue;
    }
    ParsedType dependency;
    std::string dependency_value = field_type;
    if (dependency_value.find('/') == std::string::npos) {
      dependency_value = type.package + "/msg/" + dependency_value;
    } else if (split(dependency_value, '/').size() == 2) {
      dependency_value = dependency_value.substr(0, dependency_value.find('/')) + "/msg/" +
                         dependency_value.substr(dependency_value.find('/') + 1);
    }
    if (!parse_type(dependency_value, dependency, error)) {
      return false;
    }
    if (!collect(dependency, seen, out, error)) {
      return false;
    }
  }
  return true;
}

}  // namespace

SchemaText load_message_schema(const std::string& type) {
  SchemaText schema;
  schema.type = type;
  ParsedType parsed;
  std::string error;
  if (!parse_type(type, parsed, error)) {
    schema.error = std::move(error);
    return schema;
  }

  std::set<std::string> seen;
  std::string data;
  if (!collect(parsed, seen, data, error)) {
    schema.error = std::move(error);
    return schema;
  }
  schema.encoding = "ros2msg";
  schema.data = std::move(data);
  return schema;
}

}  // namespace robot_mcap::ros2
