// A very small recursive-descent JSON reader.
//
// The device only ever parses one shape of document — the poll response from
// tardis-net — so a full JSON library would be a lot of flash for very little.
// This parses into a flat node array (no pointers to reallocate) and lets the
// caller walk it by key. It is deliberately strict: anything malformed makes
// parse() fail rather than guess, and nesting is depth-limited so a hostile
// response cannot blow the stack.
//
// Portable C++17 — no Arduino headers — so the host tests exercise exactly the
// code that runs on the device.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace jsonlite {

inline constexpr int kMaxDepth = 8;

enum class Type { Null, Bool, Number, String, Object, Array };

struct Node {
  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string text;             // string value, or key when inside an object
  std::vector<int> children;    // indices into Document::nodes
};

class Document {
 public:
  bool parse(const std::string& input) {
    nodes_.clear();
    source_ = &input;
    pos_ = 0;
    skipSpace();
    int root = parseValue(0);
    if (root < 0) return false;
    skipSpace();
    if (pos_ != input.size()) return false;  // trailing garbage
    root_ = root;
    return true;
  }

  bool valid() const { return root_ >= 0 && !nodes_.empty(); }

  // ---- reading -------------------------------------------------------------

  class View {
   public:
    View(const Document* doc, int index) : doc_(doc), index_(index) {}

    bool exists() const { return doc_ != nullptr && index_ >= 0; }
    Type type() const { return exists() ? doc_->nodes_[index_].type : Type::Null; }
    bool isNull() const { return type() == Type::Null; }

    /** Member of an object; a missing key yields a non-existent view. */
    View operator[](const std::string& key) const {
      if (!exists() || type() != Type::Object) return View(doc_, -1);
      for (int child : doc_->nodes_[index_].children) {
        if (doc_->nodes_[child].text == key) {
          const std::vector<int>& value = doc_->nodes_[child].children;
          return View(doc_, value.empty() ? -1 : value[0]);
        }
      }
      return View(doc_, -1);
    }

    std::string asString(const std::string& fallback = "") const {
      if (!exists()) return fallback;
      const Node& node = doc_->nodes_[index_];
      return node.type == Type::String ? node.text : fallback;
    }

    long asLong(long fallback = 0) const {
      if (!exists()) return fallback;
      const Node& node = doc_->nodes_[index_];
      if (node.type != Type::Number) return fallback;
      return static_cast<long>(node.number);
    }

    bool asBool(bool fallback = false) const {
      if (!exists()) return fallback;
      const Node& node = doc_->nodes_[index_];
      return node.type == Type::Bool ? node.boolean : fallback;
    }

   private:
    const Document* doc_;
    int index_;
  };

  View root() const { return View(this, root_); }
  View operator[](const std::string& key) const { return root()[key]; }

 private:
  std::vector<Node> nodes_;
  const std::string* source_ = nullptr;
  size_t pos_ = 0;
  int root_ = -1;

  char peek() const { return pos_ < source_->size() ? (*source_)[pos_] : '\0'; }

  void skipSpace() {
    while (pos_ < source_->size()) {
      char c = (*source_)[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        pos_++;
      } else {
        break;
      }
    }
  }

  bool literal(const char* text) {
    size_t length = std::string(text).size();
    if (source_->compare(pos_, length, text) != 0) return false;
    pos_ += length;
    return true;
  }

  int addNode(Node node) {
    nodes_.push_back(std::move(node));
    return static_cast<int>(nodes_.size()) - 1;
  }

  int parseValue(int depth) {
    if (depth > kMaxDepth) return -1;
    skipSpace();
    switch (peek()) {
      case '{': return parseObject(depth);
      case '[': return parseArray(depth);
      case '"': {
        std::string text;
        if (!parseString(text)) return -1;
        Node node;
        node.type = Type::String;
        node.text = std::move(text);
        return addNode(std::move(node));
      }
      case 't': {
        if (!literal("true")) return -1;
        Node node;
        node.type = Type::Bool;
        node.boolean = true;
        return addNode(std::move(node));
      }
      case 'f': {
        if (!literal("false")) return -1;
        Node node;
        node.type = Type::Bool;
        node.boolean = false;
        return addNode(std::move(node));
      }
      case 'n': {
        if (!literal("null")) return -1;
        return addNode(Node{});
      }
      default: return parseNumber();
    }
  }

  int parseNumber() {
    size_t start = pos_;
    if (peek() == '-' || peek() == '+') pos_++;
    bool digits = false;
    while (pos_ < source_->size()) {
      char c = (*source_)[pos_];
      if ((c >= '0' && c <= '9')) {
        digits = true;
        pos_++;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        pos_++;
      } else {
        break;
      }
    }
    if (!digits) return -1;
    Node node;
    node.type = Type::Number;
    node.number = std::strtod(source_->substr(start, pos_ - start).c_str(), nullptr);
    return addNode(std::move(node));
  }

  bool parseString(std::string& out) {
    if (peek() != '"') return false;
    pos_++;
    out.clear();
    while (pos_ < source_->size()) {
      char c = (*source_)[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= source_->size()) return false;
      char escape = (*source_)[pos_++];
      switch (escape) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
          if (pos_ + 4 > source_->size()) return false;
          // Only the ASCII range matters here; anything else becomes '?'.
          unsigned code = std::strtoul(source_->substr(pos_, 4).c_str(), nullptr, 16);
          pos_ += 4;
          out.push_back(code < 0x80 ? static_cast<char>(code) : '?');
          break;
        }
        default: out.push_back(escape); break;  // \" \\ \/ and friends
      }
    }
    return false;  // unterminated
  }

  int parseObject(int depth) {
    pos_++;  // '{'
    Node object;
    object.type = Type::Object;
    int index = addNode(std::move(object));

    skipSpace();
    if (peek() == '}') {
      pos_++;
      return index;
    }
    while (true) {
      skipSpace();
      std::string key;
      if (!parseString(key)) return -1;
      skipSpace();
      if (peek() != ':') return -1;
      pos_++;
      int value = parseValue(depth + 1);
      if (value < 0) return -1;

      Node member;
      member.type = Type::String;  // the member node only carries the key
      member.text = std::move(key);
      member.children.push_back(value);
      int memberIndex = addNode(std::move(member));
      nodes_[index].children.push_back(memberIndex);

      skipSpace();
      if (peek() == ',') {
        pos_++;
        continue;
      }
      if (peek() == '}') {
        pos_++;
        return index;
      }
      return -1;
    }
  }

  int parseArray(int depth) {
    pos_++;  // '['
    Node array;
    array.type = Type::Array;
    int index = addNode(std::move(array));

    skipSpace();
    if (peek() == ']') {
      pos_++;
      return index;
    }
    while (true) {
      int value = parseValue(depth + 1);
      if (value < 0) return -1;
      nodes_[index].children.push_back(value);
      skipSpace();
      if (peek() == ',') {
        pos_++;
        continue;
      }
      if (peek() == ']') {
        pos_++;
        return index;
      }
      return -1;
    }
  }
};

/** Escape a string for embedding in a JSON document we emit. */
inline std::string escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static const char* kHex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(c >> 4) & 0xf]);
          out.push_back(kHex[c & 0xf]);
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

}  // namespace jsonlite
