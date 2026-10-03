#include "JsonListParser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {
JsonListParser &self(void *ctx) { return *static_cast<JsonListParser *>(ctx); }
} // namespace

JsonListParser::JsonListParser(const std::string &itemsPath,
                               const std::string *const (&fieldPaths)[F_COUNT],
                               const std::string &filesPath, const RowSink sink,
                               void *sinkCtx)
    : sink(sink), sinkCtx(sinkCtx),
      json(JsonCallbacks{
          this,
          [](void *c, const char *k, size_t n) {
            auto &p = self(c);
            if (n > 128) {
              p.malformed = true;
              return;
            }
            if (!p.stack.empty() && !p.stack.back().isArray)
              p.stack.back().key.assign(k, n);
          },
          [](void *c, const char *v, size_t n) {
            self(c).onScalar(v, n, true);
          },
          [](void *c, const char *v, size_t n) {
            self(c).onScalar(v, n, false);
          },
          [](void *c, bool) { self(c).beginValue(); },
          [](void *c) { self(c).beginValue(); },
          [](void *c) { self(c).onContainerStart(false); },
          [](void *c) { self(c).onContainerEnd(false); },
          [](void *c) { self(c).onContainerStart(true); },
          [](void *c) { self(c).onContainerEnd(true); },
      }) {
  stack.reserve(StreamingJsonParser::MAX_NESTING);
  malformed = !split(itemsPath, items) || !split(filesPath, files);
  for (int i = 0; i < F_COUNT; i++)
    if (!split(*fieldPaths[i], fields[i]))
      malformed = true;
}

bool JsonListParser::parse(const ReadFn read, void *readCtx) {
  if (malformed)
    return false;
  for (int n; (n = read(readCtx, buf, sizeof(buf))) > 0;) {
    json.feed(buf, static_cast<size_t>(n));
    if (json.hasError() || malformed)
      return false;
  }
  // JsonSax does not check nesting; an unclosed container means a cut-off body.
  return stack.empty();
}

bool JsonListParser::split(const std::string &dotted, Path &out) {
  if (dotted.empty())
    return true;
  const size_t count = std::count(dotted.begin(), dotted.end(), '.') + 1;
  if (count > StreamingJsonParser::MAX_NESTING)
    return false;
  out.reserve(count);
  size_t start = 0;
  while (start < dotted.size()) {
    const size_t dot = dotted.find('.', start);
    const size_t end = dot == std::string::npos ? dotted.size() : dot;
    if (end > start)
      out.push_back(dotted.substr(start, end - start));
    start = end + 1;
  }
  return true;
}

bool JsonListParser::segEquals(const Frame &frame, const std::string &seg) {
  if (!frame.isArray)
    return frame.key == seg;
  return !seg.empty() && isdigit(static_cast<unsigned char>(seg[0])) &&
         atoi(seg.c_str()) == frame.index;
}

bool JsonListParser::framesMatch(const size_t from, const Path &path,
                                 const size_t extra) const {
  if (stack.size() != from + path.size() + extra)
    return false;
  for (size_t i = 0; i < path.size(); i++) {
    if (!segEquals(stack[from + i], path[i]))
      return false;
  }
  return true;
}

// Every value (scalar or container) advances its parent array's index.
void JsonListParser::beginValue() {
  if (!stack.empty() && stack.back().isArray)
    stack.back().index++;
}

// The value just begun is an element of the items array.
bool JsonListParser::atItem() const {
  if (stack.size() != items.size() + 1 || !stack.back().isArray)
    return false;
  for (size_t i = 0; i < items.size(); i++) {
    if (!segEquals(stack[i], items[i]))
      return false;
  }
  return true;
}

void JsonListParser::onContainerStart(const bool isArray) {
  if (malformed || stack.size() >= StreamingJsonParser::MAX_NESTING) {
    malformed = true;
    return;
  }
  beginValue();
  if (itemDepth == 0 && !isArray && atItem()) {
    current = Row{};
    rowBytes = 0;
    if (!files.empty())
      current.files.reserve(MAX_FILES);
    itemDepth = stack.size() + 1;
  }
  stack.push_back(Frame{isArray, -1, {}});
}

void JsonListParser::onContainerEnd(const bool isArray) {
  if (malformed)
    return;
  if (stack.empty() || stack.back().isArray != isArray) {
    malformed = true;
    return;
  }
  if (itemDepth != 0 && stack.size() == itemDepth) {
    sink(sinkCtx, current);
    itemDepth = 0;
  }
  stack.pop_back();
}

void JsonListParser::onScalar(const char *value, const size_t len,
                              const bool isString) {
  beginValue();
  if (malformed || itemDepth == 0)
    return;
  // Item-relative paths start at the item object's frame, whose current key is
  // their first segment.
  const size_t base = itemDepth - 1;
  for (int i = 0; i < F_COUNT; i++) {
    if (!fields[i].empty() && current.field[i].empty() &&
        framesMatch(base, fields[i])) {
      if (len > MAX_FIELD_CHARS || len > MAX_ROW_BYTES - rowBytes) {
        malformed = true;
        return;
      }
      rowBytes += len;
      current.field[i].assign(value, len);
    }
  }
  // A files element: the frames spell filesPath, plus the array holding it.
  if (isString && !files.empty() && stack.back().isArray &&
      framesMatch(base, files, 1)) {
    if (len > MAX_FIELD_CHARS || current.files.size() >= MAX_FILES ||
        len > MAX_ROW_BYTES - rowBytes) {
      malformed = true;
      return;
    }
    rowBytes += len;
    current.files.emplace_back(value, len);
  }
}
