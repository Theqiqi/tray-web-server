#include "json.h"

#include <cctype>
#include <cstdlib>

namespace tray_panel::json {

bool Value::IsNull() const { return std::holds_alternative<std::nullptr_t>(data); }
bool Value::IsBool() const { return std::holds_alternative<bool>(data); }
bool Value::IsNumber() const { return std::holds_alternative<double>(data); }
bool Value::IsString() const { return std::holds_alternative<std::string>(data); }
bool Value::IsArray() const { return std::holds_alternative<Array>(data); }
bool Value::IsObject() const { return std::holds_alternative<Object>(data); }
bool Value::AsBool() const { return std::get<bool>(data); }
double Value::AsNumber() const { return std::get<double>(data); }
const std::string& Value::AsString() const { return std::get<std::string>(data); }
const Array& Value::AsArray() const { return std::get<Array>(data); }
const Object& Value::AsObject() const { return std::get<Object>(data); }

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Value ParseValue() {
        SkipWhitespace();
        if (position_ >= text_.size()) throw ParseError("unexpected end of input");
        const char current = text_[position_];
        if (current == '{') return ParseObject();
        if (current == '[') return ParseArray();
        if (current == '"') return Value(ParseString());
        if (current == 't') { ExpectLiteral("true"); return Value(true); }
        if (current == 'f') { ExpectLiteral("false"); return Value(false); }
        if (current == 'n') { ExpectLiteral("null"); return Value(nullptr); }
        if (current == '-' || std::isdigit(static_cast<unsigned char>(current))) return Value(ParseNumber());
        throw ParseError("unexpected token at position " + std::to_string(position_));
    }

    void EnsureFinished() {
        SkipWhitespace();
        if (position_ != text_.size()) throw ParseError("trailing data at position " + std::to_string(position_));
    }

private:
    const std::string& text_;
    std::size_t position_ = 0;

    void SkipWhitespace() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
    }

    void Expect(char expected) {
        SkipWhitespace();
        if (position_ >= text_.size() || text_[position_] != expected) throw ParseError("expected character at position " + std::to_string(position_));
        ++position_;
    }

    void ExpectLiteral(const char* literal) {
        while (*literal != '\0') {
            if (position_ >= text_.size() || text_[position_] != *literal) throw ParseError("invalid literal at position " + std::to_string(position_));
            ++position_;
            ++literal;
        }
    }

    std::string ParseString() {
        Expect('"');
        std::string result;
        while (position_ < text_.size()) {
            char current = text_[position_++];
            if (current == '"') return result;
            if (current == '\\') {
                if (position_ >= text_.size()) throw ParseError("incomplete escape sequence");
                const char escaped = text_[position_++];
                switch (escaped) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    default: throw ParseError("unsupported escape sequence");
                }
                continue;
            }
            result.push_back(current);
        }
        throw ParseError("unterminated string");
    }

    double ParseNumber() {
        SkipWhitespace();
        const std::size_t start = position_;
        if (text_[position_] == '-') ++position_;
        while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
        }
        return std::strtod(text_.c_str() + start, nullptr);
    }

    Value ParseArray() {
        Expect('[');
        Array result;
        SkipWhitespace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return Value(result);
        }
        while (true) {
            result.push_back(ParseValue());
            SkipWhitespace();
            if (position_ < text_.size() && text_[position_] == ']') {
                ++position_;
                return Value(result);
            }
            Expect(',');
        }
    }

    Value ParseObject() {
        Expect('{');
        Object result;
        SkipWhitespace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return Value(result);
        }
        while (true) {
            SkipWhitespace();
            if (position_ >= text_.size() || text_[position_] != '"') throw ParseError("object key must be a string");
            std::string key = ParseString();
            Expect(':');
            result.emplace(std::move(key), ParseValue());
            SkipWhitespace();
            if (position_ < text_.size() && text_[position_] == '}') {
                ++position_;
                return Value(result);
            }
            Expect(',');
        }
    }
};

}  // namespace

Value Parse(const std::string& text) {
    Parser parser(text);
    Value value = parser.ParseValue();
    parser.EnsureFinished();
    return value;
}

}  // namespace tray_panel::json
