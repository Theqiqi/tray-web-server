#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace tray_panel::json {

struct Value;

using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    using Variant = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Variant data;

    Value() : data(nullptr) {}
    explicit Value(Variant value) : data(std::move(value)) {}

    bool IsNull() const;
    bool IsBool() const;
    bool IsNumber() const;
    bool IsString() const;
    bool IsArray() const;
    bool IsObject() const;

    bool AsBool() const;
    double AsNumber() const;
    const std::string& AsString() const;
    const Array& AsArray() const;
    const Object& AsObject() const;
};

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Value Parse(const std::string& text);

}  // namespace tray_panel::json
