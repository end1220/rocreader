#include "epub_flow_reader.h"

#include "epub_runtime.h"
#include "image_decode.h"
#include "runtime_log.h"

#include <SDL.h>
#ifdef HAVE_SDL2_TTF
#include <SDL_ttf.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include "filesystem_compat.h"
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef HAVE_LIBZIP
#include <zip.h>
#endif

namespace {

constexpr float kMinZoom = 0.7f;
constexpr float kMaxZoom = 2.4f;
constexpr float kZoomStep = 0.1f;
constexpr int kDefaultBaseFontPt = 18;
constexpr int kMargin = 14;
constexpr size_t kNaturalTextThreshold = 1200;
constexpr size_t kNaturalRunThreshold = 80;
constexpr size_t kLongNaturalRunThreshold = 180;
constexpr size_t kMinNaturalBlocks = 4;
constexpr size_t kImageHeavyThreshold = 24;
constexpr size_t kMaxTextTextureCacheEntries = 768;
constexpr size_t kInitialFlowDocsToLoad = 8;
constexpr size_t kMoreFlowDocsToLoad = 4;
constexpr int kLazyLoadAheadScreens = 3;

enum class FlowBlockType { Paragraph, Header, ListItem, Image, Space };

struct FlowBlock {
  FlowBlockType type = FlowBlockType::Paragraph;
  std::string text;
  std::string resource;
  std::vector<std::string> lines;
  int heading_level = 0;
  int y = 0;
  int h = 0;
  int image_w = 0;
  int image_h = 0;
  int draw_w = 0;
  int draw_h = 0;
};

struct TextTextureEntry {
  SDL_Texture *texture = nullptr;
  int w = 0;
  int h = 0;
  uint32_t last_use = 0;
};

struct ManifestItem {
  std::string href;
  std::string media_type;
};

struct ParsedPackage {
  std::string opf_dir;
  std::unordered_map<std::string, ManifestItem> id_to_item;
  std::unordered_map<std::string, std::string> href_to_media_type;
  std::vector<std::string> ordered_docs;
  std::vector<std::string> manifest_html_docs;
};

using AttrMap = std::unordered_map<std::string, std::string>;

std::string TagNameFromRaw(const std::string &raw, bool &closing);
bool IsBlockTag(const std::string &tag);

std::string NormalizeZipPath(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  std::vector<std::string> parts;
  size_t cursor = 0;
  while (cursor <= path.size()) {
    const size_t slash = path.find('/', cursor);
    std::string part = path.substr(cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
    if (part.empty() || part == ".") {
    } else if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else {
      parts.push_back(std::move(part));
    }
    if (slash == std::string::npos) break;
    cursor = slash + 1;
  }
  std::string out;
  for (const auto &part : parts) {
    if (!out.empty()) out.push_back('/');
    out += part;
  }
  return out;
}

std::string ResolveRelative(const std::string &base_dir, std::string href) {
  const size_t hash = href.find('#');
  if (hash != std::string::npos) href.erase(hash);
  const size_t query = href.find('?');
  if (query != std::string::npos) href.erase(query);
  if (href.empty()) return NormalizeZipPath(base_dir);
  if (base_dir.empty()) return NormalizeZipPath(href);
  return NormalizeZipPath(base_dir + "/" + href);
}

std::string LowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string TrimSpaces(const std::string &s) {
  size_t first = 0;
  while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
  size_t last = s.size();
  while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;
  return s.substr(first, last - first);
}

std::string DecodeHtmlEntities(std::string s) {
  const std::pair<const char *, const char *> table[] = {
      {"&nbsp;", " "}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"},
      {"&quot;", "\""}, {"&apos;", "'"},
  };
  for (const auto &kv : table) {
    size_t pos = 0;
    while ((pos = s.find(kv.first, pos)) != std::string::npos) {
      s.replace(pos, std::strlen(kv.first), kv.second);
      pos += std::strlen(kv.second);
    }
  }
  return s;
}

AttrMap ParseTagAttrs(const std::string &attrs_raw) {
  AttrMap attrs;
  const std::regex attr_re("([A-Za-z_:][-A-Za-z0-9_:.]*)\\s*=\\s*(['\"])(.*?)\\2", std::regex::icase);
  for (std::sregex_iterator it(attrs_raw.begin(), attrs_raw.end(), attr_re), end; it != end; ++it) {
    attrs[LowerAscii((*it)[1].str())] = DecodeHtmlEntities((*it)[3].str());
  }
  return attrs;
}

bool PickFirstMatch(const std::string &src, const std::regex &re, std::string &out) {
  std::smatch m;
  if (!std::regex_search(src, m, re) || m.size() < 2) return false;
  out = m[1].str();
  return true;
}

bool IsHtmlMediaType(const std::string &media_type) {
  return media_type == "application/xhtml+xml" || media_type == "text/html";
}

bool IsLikelyContentCodepoint(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 'A' && cp <= 'Z') ||
         (cp >= 'a' && cp <= 'z') ||
         (cp >= '0' && cp <= '9');
}

uint32_t DecodeUtf8Codepoint(const std::string &s, size_t i, size_t len) {
  const unsigned char c0 = static_cast<unsigned char>(s[i]);
  if (len == 1) return c0;
  if (len == 2 && i + 1 < s.size()) {
    return ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
  }
  if (len == 3 && i + 2 < s.size()) {
    return ((c0 & 0x0Fu) << 12) |
           ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
  }
  if (len == 4 && i + 3 < s.size()) {
    return ((c0 & 0x07u) << 18) |
           ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
           ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
  }
  return c0;
}

size_t Utf8CharLen(unsigned char c) {
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

std::vector<std::string> WrapUtf8Text(const std::string &text, int chars_per_line) {
  std::vector<std::string> lines;
  std::string line;
  int count = 0;
  for (size_t i = 0; i < text.size();) {
    const size_t len = std::min(Utf8CharLen(static_cast<unsigned char>(text[i])), text.size() - i);
    const std::string ch = text.substr(i, len);
    const bool hard_break = (ch == "\n");
    if (hard_break || count >= chars_per_line) {
      std::string trimmed = TrimSpaces(line);
      if (!trimmed.empty()) lines.push_back(std::move(trimmed));
      line.clear();
      count = 0;
      if (hard_break) {
        i += len;
        continue;
      }
    }
    line += ch;
    if (!std::isspace(static_cast<unsigned char>(text[i]))) ++count;
    i += len;
  }
  std::string trimmed = TrimSpaces(line);
  if (!trimmed.empty()) lines.push_back(std::move(trimmed));
  if (lines.empty()) lines.push_back("");
  return lines;
}

struct NaturalTextStats {
  size_t natural_chars = 0;
  size_t natural_blocks = 0;
  size_t long_natural_blocks = 0;
  size_t max_run = 0;
};

NaturalTextStats MeasureNaturalText(const std::string &text) {
  NaturalTextStats stats;
  size_t run = 0;
  auto finish_run = [&]() {
    stats.max_run = std::max(stats.max_run, run);
    if (run >= kNaturalRunThreshold) {
      stats.natural_chars += run;
      ++stats.natural_blocks;
      if (run >= kLongNaturalRunThreshold) ++stats.long_natural_blocks;
    }
    run = 0;
  };
  for (size_t i = 0; i < text.size();) {
    const size_t len = std::min(Utf8CharLen(static_cast<unsigned char>(text[i])), text.size() - i);
    const uint32_t cp = DecodeUtf8Codepoint(text, i, len);
    if (IsLikelyContentCodepoint(cp)) {
      ++run;
    } else if (cp == 0x3002 || cp == 0xFF0C || cp == 0xFF1B || cp == '.' || cp == ',' || cp == ';' ||
               std::isspace(static_cast<unsigned char>(text[i]))) {
      if (run > 0 && run < kNaturalRunThreshold && cp != '\n') {
        // keep sentence punctuation inside a candidate paragraph run
      } else {
        finish_run();
      }
    } else {
      finish_run();
    }
    i += len;
  }
  finish_run();
  return stats;
}

size_t CountImgTagsLinear(const std::string &html) {
  size_t count = 0;
  size_t cursor = 0;
  while (cursor < html.size()) {
    const size_t lt = html.find('<', cursor);
    if (lt == std::string::npos) break;
    const size_t gt = html.find('>', lt + 1);
    if (gt == std::string::npos) break;
    bool closing = false;
    if (TagNameFromRaw(html.substr(lt + 1, gt - lt - 1), closing) == "img" && !closing) ++count;
    cursor = gt + 1;
  }
  return count;
}

#ifdef HAVE_LIBZIP
bool ReadZipEntry(zip_t *za, const std::string &name, std::string &out) {
  zip_stat_t st{};
  zip_int64_t index = zip_name_locate(za, name.c_str(), 0);
  if (index < 0) index = zip_name_locate(za, name.c_str(), ZIP_FL_NOCASE);
  if (index < 0) return false;
  if (zip_stat_index(za, static_cast<zip_uint64_t>(index), 0, &st) != 0) return false;
  if (st.size > static_cast<zip_uint64_t>(128 * 1024 * 1024)) return false;
  zip_file_t *zf = zip_fopen_index(za, static_cast<zip_uint64_t>(index), 0);
  if (!zf) return false;
  out.resize(static_cast<size_t>(st.size));
  size_t total = 0;
  while (total < out.size()) {
    const zip_int64_t rd = zip_fread(zf, out.data() + total, out.size() - total);
    if (rd < 0) {
      zip_fclose(zf);
      out.clear();
      return false;
    }
    if (rd == 0) break;
    total += static_cast<size_t>(rd);
  }
  zip_fclose(zf);
  out.resize(total);
  return !out.empty();
}

bool ParsePackage(zip_t *za, ParsedPackage &pkg, std::string &error) {
  std::string container_xml;
  if (!ReadZipEntry(za, "META-INF/container.xml", container_xml)) {
    error = "missing META-INF/container.xml";
    return false;
  }
  std::string rootfile_tag;
  if (!PickFirstMatch(container_xml, std::regex("<rootfile\\b([^>]*)>", std::regex::icase), rootfile_tag)) {
    error = "cannot locate rootfile";
    return false;
  }
  AttrMap root_attrs = ParseTagAttrs(rootfile_tag);
  const auto full_it = root_attrs.find("full-path");
  if (full_it == root_attrs.end() || full_it->second.empty()) {
    error = "rootfile full-path missing";
    return false;
  }
  const std::string opf_path = full_it->second;
  std::string opf;
  if (!ReadZipEntry(za, opf_path, opf)) {
    error = "cannot read OPF";
    return false;
  }
  try {
    pkg.opf_dir = std::filesystem::path(opf_path).parent_path().generic_string();
  } catch (...) {
    pkg.opf_dir.clear();
  }

  const std::regex item_re("<item\\b([^>]*)>", std::regex::icase);
  for (std::sregex_iterator it(opf.begin(), opf.end(), item_re), end; it != end; ++it) {
    AttrMap attrs = ParseTagAttrs((*it)[1].str());
    const auto id_it = attrs.find("id");
    const auto href_it = attrs.find("href");
    if (id_it == attrs.end() || href_it == attrs.end() || id_it->second.empty() || href_it->second.empty()) continue;
    ManifestItem item;
    item.href = ResolveRelative(pkg.opf_dir, href_it->second);
    const auto mt_it = attrs.find("media-type");
    if (mt_it != attrs.end()) item.media_type = mt_it->second;
    pkg.id_to_item[id_it->second] = item;
    pkg.href_to_media_type[item.href] = item.media_type;
    if (IsHtmlMediaType(item.media_type)) pkg.manifest_html_docs.push_back(item.href);
  }

  const std::regex spine_re("<itemref\\b[^>]*idref\\s*=\\s*['\"]([^'\"]+)['\"][^>]*>", std::regex::icase);
  for (std::sregex_iterator it(opf.begin(), opf.end(), spine_re), end; it != end; ++it) {
    const auto mit = pkg.id_to_item.find((*it)[1].str());
    if (mit == pkg.id_to_item.end() || !IsHtmlMediaType(mit->second.media_type)) continue;
    pkg.ordered_docs.push_back(mit->second.href);
  }
  if (pkg.ordered_docs.empty()) pkg.ordered_docs = pkg.manifest_html_docs;
  if (pkg.ordered_docs.empty()) {
    error = "no html/xhtml spine content";
    return false;
  }
  return true;
}
#endif

std::string StripTagsToText(std::string html) {
  std::string out;
  out.reserve(html.size());
  bool in_tag = false;
  bool last_space = true;
  for (size_t i = 0; i < html.size();) {
    const char ch = html[i];
    if (ch == '<') {
      const size_t gt = html.find('>', i + 1);
      if (gt == std::string::npos) break;
      const std::string raw = html.substr(i + 1, gt - i - 1);
      bool closing = false;
      const std::string tag = TagNameFromRaw(raw, closing);
      if (!closing && (tag == "script" || tag == "style")) {
        const std::string lower_tail = LowerAscii(html.substr(gt + 1));
        const std::string close_pat = "</" + tag;
        const size_t close_pos = lower_tail.find(close_pat);
        if (close_pos != std::string::npos) {
          const size_t close_start = gt + 1 + close_pos;
          const size_t close_end = html.find('>', close_start);
          i = (close_end == std::string::npos) ? html.size() : close_end + 1;
          continue;
        }
      }
      if (tag == "br" || IsBlockTag(tag)) {
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        last_space = true;
      } else if (!last_space) {
        out.push_back(' ');
        last_space = true;
      }
      i = gt + 1;
      continue;
    }
    in_tag = false;
    (void)in_tag;
    if (ch == '&') {
      const size_t semi = html.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 12) {
        std::string entity = DecodeHtmlEntities(html.substr(i, semi - i + 1));
        if (entity.size() < semi - i + 1) {
          for (char ec : entity) {
            if (std::isspace(static_cast<unsigned char>(ec))) {
              if (!last_space) out.push_back(' ');
              last_space = true;
            } else {
              out.push_back(ec);
              last_space = false;
            }
          }
          i = semi + 1;
          continue;
        }
      }
    }
    const size_t len = std::min(Utf8CharLen(static_cast<unsigned char>(ch)), html.size() - i);
    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (ch == '\n') {
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
      } else if (!last_space) {
        out.push_back(' ');
      }
      last_space = true;
    } else {
      out.append(html, i, len);
      last_space = false;
    }
    i += len;
  }
  return TrimSpaces(out);
}

void PushTextBlock(std::vector<FlowBlock> &blocks, FlowBlockType type, std::string text, int heading_level = 0) {
  text = StripTagsToText(std::move(text));
  if (text.empty()) return;
  FlowBlock block;
  block.type = type;
  block.text = std::move(text);
  block.heading_level = heading_level;
  blocks.push_back(std::move(block));
}

void PushImageBlock(std::vector<FlowBlock> &blocks, const std::string &doc_base, const std::string &attrs_raw) {
  AttrMap attrs = ParseTagAttrs(attrs_raw);
  const auto src_it = attrs.find("src");
  if (src_it == attrs.end() || src_it->second.empty()) return;
  FlowBlock block;
  block.type = FlowBlockType::Image;
  block.resource = ResolveRelative(doc_base, src_it->second);
  blocks.push_back(std::move(block));
}

std::string TagNameFromRaw(const std::string &raw, bool &closing) {
  closing = false;
  size_t i = 0;
  while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
  if (i < raw.size() && raw[i] == '/') {
    closing = true;
    ++i;
    while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
  }
  const size_t start = i;
  while (i < raw.size() && std::isalnum(static_cast<unsigned char>(raw[i]))) ++i;
  return LowerAscii(raw.substr(start, i - start));
}

std::string AttrsRawFromTagRaw(const std::string &raw) {
  size_t i = 0;
  while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
  if (i < raw.size() && raw[i] == '/') ++i;
  while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
  while (i < raw.size() && std::isalnum(static_cast<unsigned char>(raw[i]))) ++i;
  return raw.substr(i);
}

bool IsBlockTag(const std::string &tag) {
  return tag == "p" || tag == "div" || tag == "li" ||
         (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6');
}

FlowBlockType TypeForTag(const std::string &tag) {
  if (tag == "li") return FlowBlockType::ListItem;
  if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') return FlowBlockType::Header;
  return FlowBlockType::Paragraph;
}

int HeadingLevelForTag(const std::string &tag) {
  if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') return tag[1] - '0';
  return 0;
}

void FlushFlowText(std::vector<FlowBlock> &blocks,
                   std::string &text,
                   FlowBlockType type,
                   int heading_level) {
  PushTextBlock(blocks, type, text, heading_level);
  text.clear();
}

void ParseHtmlBlocks(const std::string &html, const std::string &doc_path, std::vector<FlowBlock> &blocks) {
  const std::string doc_base = std::filesystem::path(doc_path).parent_path().generic_string();
  FlowBlockType current_type = FlowBlockType::Paragraph;
  int current_heading = 0;
  std::string text;
  size_t cursor = 0;
  while (cursor < html.size()) {
    const size_t lt = html.find('<', cursor);
    if (lt == std::string::npos) {
      text.append(html, cursor, std::string::npos);
      break;
    }
    text.append(html, cursor, lt - cursor);
    const size_t gt = html.find('>', lt + 1);
    if (gt == std::string::npos) break;
    const std::string raw = html.substr(lt + 1, gt - lt - 1);
    bool closing = false;
    const std::string tag = TagNameFromRaw(raw, closing);
    const std::string attrs_raw = AttrsRawFromTagRaw(raw);

    if (!closing && (tag == "script" || tag == "style")) {
      const std::string lower_tail = LowerAscii(html.substr(gt + 1));
      const std::string close_pat = "</" + tag;
      const size_t close_pos = lower_tail.find(close_pat);
      if (close_pos != std::string::npos) {
        const size_t close_start = gt + 1 + close_pos;
        const size_t close_end = html.find('>', close_start);
        cursor = (close_end == std::string::npos) ? html.size() : close_end + 1;
        continue;
      }
    }

    if (!closing && tag == "br") {
      text.push_back('\n');
    } else if (!closing && tag == "img") {
      FlushFlowText(blocks, text, current_type, current_heading);
      PushImageBlock(blocks, doc_base, attrs_raw);
    } else if (!closing && IsBlockTag(tag)) {
      FlushFlowText(blocks, text, current_type, current_heading);
      current_type = TypeForTag(tag);
      current_heading = HeadingLevelForTag(tag);
    } else if (closing && IsBlockTag(tag)) {
      FlushFlowText(blocks, text, current_type, current_heading);
      current_type = FlowBlockType::Paragraph;
      current_heading = 0;
    }
    cursor = gt + 1;
  }
  FlushFlowText(blocks, text, current_type, current_heading);
}

int NormalizeRotation(int rotation) {
  rotation %= 360;
  if (rotation < 0) rotation += 360;
  return rotation;
}

}  // namespace

struct EpubFlowReader::Impl {
  SDL_Renderer *renderer = nullptr;
  std::string path;
  int screen_w = 720;
  int screen_h = 480;
  int base_font_pt = kDefaultBaseFontPt;
  SDL_Color background_color{250, 249, 244, 255};
  SDL_Color font_color{28, 28, 28, 255};
  int rotation = 0;
  float zoom = 1.0f;
  int scroll_y = 0;
  int doc_h = 1;
  std::vector<std::string> ordered_docs;
  size_t next_doc_index = 0;
  bool all_docs_loaded = true;
  std::vector<FlowBlock> source_blocks;
  mutable std::unordered_map<std::string, SDL_Texture *> image_textures;
  mutable std::unordered_map<std::string, SDL_Point> image_sizes;
  mutable std::unordered_map<std::string, TextTextureEntry> text_textures;
#ifdef HAVE_SDL2_TTF
  TTF_Font *font = nullptr;
  TTF_Font *header_font = nullptr;
#endif

  int LayoutWidth() const {
    const int w = (rotation == 90 || rotation == 270) ? screen_h : screen_w;
    return std::max(80, w - kMargin * 2);
  }

  int ViewWidth() const {
    return std::max(1, (rotation == 90 || rotation == 270) ? screen_h : screen_w);
  }

  int ViewHeight() const {
    return std::max(1, (rotation == 90 || rotation == 270) ? screen_w : screen_h);
  }

  int FontPt() const { return std::max(8, static_cast<int>(std::lround(base_font_pt * zoom))); }

  bool FontHasChineseGlyph(const std::string &path, int pt) const {
#ifdef HAVE_SDL2_TTF
    TTF_Font *probe = TTF_OpenFont(path.c_str(), std::max(8, pt));
    if (!probe) return false;
    const bool ok = TTF_GlyphIsProvided(probe, 0x660E) != 0 &&
                    TTF_GlyphIsProvided(probe, 0x671D) != 0;
    TTF_CloseFont(probe);
    return ok;
#else
    (void)path;
    (void)pt;
    return false;
#endif
  }

  std::string FontPath() const {
    const std::vector<std::string> candidates = {
        "fonts/ui_font_02.ttf", "resources/fonts/ui_font_02.ttf",
        "fonts/ui_font.ttf", "resources/fonts/ui_font.ttf",
        "resources/fonts/default.ttf", "default.ttf",
        "/Roms/APPS/ROCreader/fonts/ui_font_02.ttf",
        "/Roms/APPS/ROCreader/fonts/ui_font.ttf",
        "/mnt/mmc/ROCreader/fonts/ui_font_02.ttf",
        "/mnt/mmc/ROCreader/fonts/ui_font.ttf",
        "/mnt/mmc/Roms/ROCreader/fonts/ui_font_02.ttf",
        "/mnt/mmc/Roms/ROCreader/fonts/ui_font.ttf",
        "/mnt/mmc2/ROCreader/fonts/ui_font_02.ttf",
        "/mnt/mmc2/ROCreader/fonts/ui_font.ttf",
        "/mnt/mmc2/Roms/ROCreader/fonts/ui_font_02.ttf",
        "/mnt/mmc2/Roms/ROCreader/fonts/ui_font.ttf",
        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/simsun.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    };
    std::string fallback;
    for (const auto &candidate : candidates) {
      std::error_code ec;
      if (!std::filesystem::exists(candidate, ec) || ec) continue;
      if (fallback.empty()) fallback = candidate;
      if (FontHasChineseGlyph(candidate, FontPt())) return candidate;
    }
    return fallback;
  }

  void CloseFonts() {
#ifdef HAVE_SDL2_TTF
    if (font) TTF_CloseFont(font);
    if (header_font) TTF_CloseFont(header_font);
    font = nullptr;
    header_font = nullptr;
#endif
  }

  void EnsureFonts() {
#ifdef HAVE_SDL2_TTF
    const std::string fp = FontPath();
    if (fp.empty()) return;
    const int body_pt = FontPt();
    if (!font) font = TTF_OpenFont(fp.c_str(), body_pt);
    if (!header_font) header_font = TTF_OpenFont(fp.c_str(), std::max(body_pt + 4, static_cast<int>(body_pt * 1.25f)));
    runtime_log::Line("[epub_flow] font path=" + fp +
                      " chinese=" + (FontHasChineseGlyph(fp, body_pt) ? "yes" : "no"));
#endif
  }

  void DestroyImages() const {
    for (auto &kv : image_textures) {
      if (kv.second) SDL_DestroyTexture(kv.second);
    }
    image_textures.clear();
    image_sizes.clear();
  }

  void DestroyTextTextures() const {
    for (auto &kv : text_textures) {
      if (kv.second.texture) SDL_DestroyTexture(kv.second.texture);
    }
    text_textures.clear();
  }

  void PruneTextTextures() const {
    while (text_textures.size() > kMaxTextTextureCacheEntries) {
      auto oldest = text_textures.end();
      for (auto it = text_textures.begin(); it != text_textures.end(); ++it) {
        if (oldest == text_textures.end() || it->second.last_use < oldest->second.last_use) oldest = it;
      }
      if (oldest == text_textures.end()) break;
      if (oldest->second.texture) SDL_DestroyTexture(oldest->second.texture);
      text_textures.erase(oldest);
    }
  }

  bool ReadImageSize(const std::string &resource, int &w, int &h) {
    w = 0;
    h = 0;
    const auto cached = image_sizes.find(resource);
    if (cached != image_sizes.end()) {
      w = cached->second.x;
      h = cached->second.y;
      return w > 0 && h > 0;
    }
#ifdef HAVE_LIBZIP
    int zerr = 0;
    zip_t *za = zip_open(path.c_str(), ZIP_RDONLY, &zerr);
    if (!za) return false;
    std::string bytes;
    const bool ok = ReadZipEntry(za, resource, bytes);
    zip_close(za);
    if (!ok || bytes.empty()) return false;
    SDL_Surface *surface = DecodeSurfaceFromMemory(bytes.data(), bytes.size());
    if (!surface) return false;
    w = surface->w;
    h = surface->h;
    SDL_FreeSurface(surface);
    if (w <= 0 || h <= 0) return false;
    image_sizes[resource] = SDL_Point{w, h};
    return true;
#else
    (void)resource;
    return false;
#endif
  }

  void Relayout(bool clear_images = true, bool clear_text_cache = true) {
    if (clear_images) DestroyImages();
    if (clear_text_cache) {
      DestroyTextTextures();
      CloseFonts();
      EnsureFonts();
    } else if (!font || !header_font) {
      EnsureFonts();
    }
    const int width = LayoutWidth();
    const int image_width = ViewWidth();
    int y = kMargin;
    const int line_h = std::max(16, static_cast<int>(std::lround(FontPt() * 1.45f)));
    for (auto &block : source_blocks) {
      block.y = y;
      block.h = 0;
      if (block.type == FlowBlockType::Image) {
        int iw = 0;
        int ih = 0;
        block.draw_w = image_width;
        if (ReadImageSize(block.resource, iw, ih)) {
          block.image_w = iw;
          block.image_h = ih;
          block.draw_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(block.draw_w) * ih / iw)));
        } else {
          block.draw_h = std::max(line_h * 4, static_cast<int>(image_width * 0.62f));
        }
        y += block.draw_h + line_h / 2;
        block.h = block.draw_h + line_h / 2;
      } else {
        const int chars_per_line = std::max(4, width / std::max(8, FontPt()));
        std::string prefix = block.type == FlowBlockType::ListItem ? "- " : "";
        block.lines = WrapUtf8Text(prefix + block.text, chars_per_line);
        const int lines = std::max(1, static_cast<int>(block.lines.size()));
        const int gap = (block.type == FlowBlockType::Header) ? line_h : line_h / 2;
        block.h = lines * line_h + gap;
        y += block.h;
      }
    }
    doc_h = std::max(ViewHeight(), y + kMargin);
    scroll_y = std::clamp(scroll_y, 0, MaxScroll());
  }

  int MaxScroll() const { return std::max(0, doc_h - ViewHeight()); }

#ifdef HAVE_SDL2_TTF
  TTF_Font *FontForBlock(const FlowBlock &block) const {
    return block.type == FlowBlockType::Header && header_font ? header_font : font;
  }
#endif

  void DrawTextLine(SDL_Renderer *r, const std::string &text, int x, int y, const FlowBlock &block) const {
#ifdef HAVE_SDL2_TTF
    TTF_Font *draw_font = FontForBlock(block);
    if (!draw_font || text.empty()) return;
    const char role = block.type == FlowBlockType::Header ? 'h' : 'b';
    const std::string key = std::string(1, role) + "|" + std::to_string(FontPt()) + "|" +
                            std::to_string(font_color.r) + "," + std::to_string(font_color.g) + "," +
                            std::to_string(font_color.b) + "," + text;
    auto cached = text_textures.find(key);
    if (cached == text_textures.end()) {
      SDL_Surface *surface = TTF_RenderUTF8_Blended(draw_font, text.c_str(), font_color);
      if (!surface) return;
      TextTextureEntry entry;
      entry.texture = SDL_CreateTextureFromSurface(r, surface);
      entry.w = surface->w;
      entry.h = surface->h;
      entry.last_use = SDL_GetTicks();
      SDL_FreeSurface(surface);
      if (!entry.texture) return;
      cached = text_textures.emplace(key, entry).first;
      PruneTextTextures();
    }
    cached->second.last_use = SDL_GetTicks();
    SDL_Rect dst{x, y, cached->second.w, cached->second.h};
    SDL_RenderCopy(r, cached->second.texture, nullptr, &dst);
#else
    (void)r;
    (void)text;
    (void)x;
    (void)y;
    (void)block;
#endif
  }

  void DrawTextBlock(SDL_Renderer *r, const FlowBlock &block, int y) const {
    const int line_h = std::max(16, static_cast<int>(std::lround(FontPt() * 1.45f)));
    int line_y = y;
    for (const auto &line : block.lines) {
      DrawTextLine(r, line, kMargin, line_y, block);
      line_y += line_h;
    }
  }

  SDL_Texture *LoadImage(SDL_Renderer *r, const FlowBlock &block, int &w, int &h) const {
    const auto size_it = image_sizes.find(block.resource);
    if (size_it != image_sizes.end()) {
      w = size_it->second.x;
      h = size_it->second.y;
    }
    const auto tex_it = image_textures.find(block.resource);
    if (tex_it != image_textures.end()) return tex_it->second;
#ifdef HAVE_LIBZIP
    int zerr = 0;
    zip_t *za = zip_open(path.c_str(), ZIP_RDONLY, &zerr);
    if (!za) return nullptr;
    std::string bytes;
    const bool ok = ReadZipEntry(za, block.resource, bytes);
    zip_close(za);
    if (!ok || bytes.empty()) return nullptr;
    SDL_Surface *surface = DecodeSurfaceFromMemory(bytes.data(), bytes.size());
    if (!surface) return nullptr;
    w = surface->w;
    h = surface->h;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(r, surface);
    SDL_FreeSurface(surface);
    image_textures[block.resource] = texture;
    image_sizes[block.resource] = SDL_Point{w, h};
    return texture;
#else
    (void)r;
    return nullptr;
#endif
  }

  bool LoadMoreDocs(size_t max_docs) {
#ifdef HAVE_LIBZIP
    if (all_docs_loaded || max_docs == 0) return false;
    int zerr = 0;
    zip_t *za = zip_open(path.c_str(), ZIP_RDONLY, &zerr);
    if (!za) return false;
    const size_t before_blocks = source_blocks.size();
    size_t loaded = 0;
    while (next_doc_index < ordered_docs.size() && loaded < max_docs) {
      const std::string doc = ordered_docs[next_doc_index++];
      std::string html;
      if (ReadZipEntry(za, doc, html)) {
        ParseHtmlBlocks(html, doc, source_blocks);
        ++loaded;
      }
    }
    zip_close(za);
    all_docs_loaded = next_doc_index >= ordered_docs.size();
    if (source_blocks.size() == before_blocks) return false;
    Relayout(false, false);
    runtime_log::Line("[epub_flow] lazy loaded docs=" + std::to_string(loaded) +
                      " blocks=" + std::to_string(source_blocks.size()) +
                      " next_doc=" + std::to_string(next_doc_index) + "/" +
                      std::to_string(ordered_docs.size()));
    return true;
#else
    (void)max_docs;
    return false;
#endif
  }

  void LoadAheadIfNeeded() {
    if (all_docs_loaded) return;
    const int threshold = ViewHeight() * kLazyLoadAheadScreens;
    if (doc_h - (scroll_y + ViewHeight()) <= threshold) {
      LoadMoreDocs(kMoreFlowDocsToLoad);
    }
  }
};

EpubFlowReader::EpubFlowReader() : impl_(new Impl()) {}

EpubFlowReader::~EpubFlowReader() {
  Close();
  delete impl_;
  impl_ = nullptr;
}

bool EpubFlowReader::Open(const std::string &path, SDL_Renderer *renderer, int screen_w, int screen_h,
                          const EpubRuntimeProgress &initial_progress, int base_font_pt,
                          SDL_Color background_color, SDL_Color font_color) {
  Close();
#if !defined(HAVE_LIBZIP) || !defined(HAVE_SDL2_TTF)
  (void)path;
  (void)renderer;
  (void)screen_w;
  (void)screen_h;
  (void)initial_progress;
  (void)base_font_pt;
  (void)background_color;
  (void)font_color;
  return false;
#else
  int zerr = 0;
  zip_t *za = zip_open(path.c_str(), ZIP_RDONLY, &zerr);
  if (!za) return false;
  ParsedPackage pkg;
  std::string error;
  if (!ParsePackage(za, pkg, error)) {
    zip_close(za);
    runtime_log::Line("[epub_flow] package parse failed path=" + path + " error=" + error);
    return false;
  }
  impl_->path = path;
  impl_->ordered_docs = pkg.ordered_docs;
  impl_->next_doc_index = 0;
  impl_->all_docs_loaded = impl_->ordered_docs.empty();
  std::vector<FlowBlock> blocks;
  size_t docs_loaded = 0;
  while (impl_->next_doc_index < impl_->ordered_docs.size() && docs_loaded < kInitialFlowDocsToLoad) {
    const auto &doc = impl_->ordered_docs[impl_->next_doc_index++];
    std::string html;
    if (!ReadZipEntry(za, doc, html)) continue;
    ParseHtmlBlocks(html, doc, blocks);
    ++docs_loaded;
    if (blocks.size() > 1800) break;
  }
  impl_->all_docs_loaded = impl_->next_doc_index >= impl_->ordered_docs.size();
  zip_close(za);
  if (blocks.empty()) {
    runtime_log::Line("[epub_flow] open failed: no flow blocks path=" + path);
    impl_->path.clear();
    impl_->ordered_docs.clear();
    return false;
  }
  impl_->renderer = renderer;
  impl_->screen_w = std::max(1, screen_w);
  impl_->screen_h = std::max(1, screen_h);
  impl_->base_font_pt = std::max(8, base_font_pt);
  impl_->background_color = background_color;
  impl_->font_color = font_color;
  impl_->rotation = NormalizeRotation(initial_progress.rotation);
  impl_->zoom = std::clamp(initial_progress.zoom, kMinZoom, kMaxZoom);
  impl_->source_blocks = std::move(blocks);
  impl_->Relayout();
  const int resume_scroll = std::max(initial_progress.scroll_y,
                                    std::max(0, initial_progress.page) * impl_->ViewHeight());
  while (!impl_->all_docs_loaded && resume_scroll > impl_->MaxScroll() - impl_->ViewHeight()) {
    if (!impl_->LoadMoreDocs(kMoreFlowDocsToLoad)) break;
  }
  impl_->scroll_y = std::clamp(std::max(0, resume_scroll), 0, impl_->MaxScroll());
  impl_->LoadAheadIfNeeded();
  runtime_log::Line("[epub_flow] open ok blocks=" + std::to_string(impl_->source_blocks.size()) +
                    " doc_h=" + std::to_string(impl_->doc_h) +
                    " docs=" + std::to_string(docs_loaded) + "/" +
                    std::to_string(impl_->ordered_docs.size()));
  return true;
#endif
}

void EpubFlowReader::Close() {
  if (!impl_) return;
  impl_->DestroyImages();
  impl_->DestroyTextTextures();
  impl_->CloseFonts();
  impl_->path.clear();
  impl_->ordered_docs.clear();
  impl_->next_doc_index = 0;
  impl_->all_docs_loaded = true;
  impl_->source_blocks.clear();
  impl_->scroll_y = 0;
  impl_->doc_h = 1;
  impl_->base_font_pt = kDefaultBaseFontPt;
  impl_->background_color = SDL_Color{250, 249, 244, 255};
  impl_->font_color = SDL_Color{28, 28, 28, 255};
}

bool EpubFlowReader::IsOpen() const { return impl_ && !impl_->path.empty(); }
bool EpubFlowReader::HasRealRenderer() const { return true; }
const char *EpubFlowReader::BackendName() const { return "epub-flow"; }
bool EpubFlowReader::IsRenderPending() const { return false; }

void EpubFlowReader::UpdateViewport(int screen_w, int screen_h) {
  if (!IsOpen()) return;
  const float pct = impl_->MaxScroll() > 0 ? static_cast<float>(impl_->scroll_y) / impl_->MaxScroll() : 0.0f;
  impl_->screen_w = std::max(1, screen_w);
  impl_->screen_h = std::max(1, screen_h);
  impl_->scroll_y = 0;
  impl_->Relayout();
  impl_->scroll_y = std::clamp(static_cast<int>(std::lround(pct * impl_->MaxScroll())), 0, impl_->MaxScroll());
}

void EpubFlowReader::Tick() {
  if (!IsOpen()) return;
  impl_->LoadAheadIfNeeded();
}

void EpubFlowReader::Draw(SDL_Renderer *renderer) const {
  if (!IsOpen() || !renderer) return;
  SDL_SetRenderDrawColor(renderer, impl_->background_color.r, impl_->background_color.g,
                         impl_->background_color.b, impl_->background_color.a);
  SDL_Rect bg{0, 0, impl_->screen_w, impl_->screen_h};
  SDL_RenderFillRect(renderer, &bg);
  const int view_h = impl_->ViewHeight();
  SDL_Rect clip{0, 0, impl_->screen_w, impl_->screen_h};
  SDL_RenderSetClipRect(renderer, &clip);
  for (const auto &block : impl_->source_blocks) {
    const int y = block.y - impl_->scroll_y;
    if (y > view_h || y + block.h < 0) continue;
    if (block.type == FlowBlockType::Image) {
      int iw = 0;
      int ih = 0;
      SDL_Texture *texture = impl_->LoadImage(renderer, block, iw, ih);
      SDL_Rect dst{std::max(0, (impl_->screen_w - block.draw_w) / 2), y, block.draw_w, block.draw_h};
      if (texture && iw > 0 && ih > 0) {
        const float scale = std::min(static_cast<float>(block.draw_w) / iw,
                                     static_cast<float>(block.draw_h) / ih);
        dst.w = std::max(1, static_cast<int>(iw * scale));
        dst.h = std::max(1, static_cast<int>(ih * scale));
        dst.x = kMargin + (block.draw_w - dst.w) / 2;
        SDL_RenderCopy(renderer, texture, nullptr, &dst);
      } else {
        SDL_SetRenderDrawColor(renderer, 230, 226, 218, 255);
        SDL_RenderFillRect(renderer, &dst);
      }
    } else {
      impl_->DrawTextBlock(renderer, block, y);
    }
  }
  SDL_RenderSetClipRect(renderer, nullptr);
}

void EpubFlowReader::RotateLeft() {
  if (!IsOpen()) return;
  const float pct = impl_->MaxScroll() > 0 ? static_cast<float>(impl_->scroll_y) / impl_->MaxScroll() : 0.0f;
  impl_->rotation = NormalizeRotation(impl_->rotation + 270);
  impl_->Relayout();
  impl_->scroll_y = std::clamp(static_cast<int>(std::lround(pct * impl_->MaxScroll())), 0, impl_->MaxScroll());
}

void EpubFlowReader::RotateRight() {
  if (!IsOpen()) return;
  const float pct = impl_->MaxScroll() > 0 ? static_cast<float>(impl_->scroll_y) / impl_->MaxScroll() : 0.0f;
  impl_->rotation = NormalizeRotation(impl_->rotation + 90);
  impl_->Relayout();
  impl_->scroll_y = std::clamp(static_cast<int>(std::lround(pct * impl_->MaxScroll())), 0, impl_->MaxScroll());
}

void EpubFlowReader::ZoomOut() {
  if (!IsOpen()) return;
  const float pct = impl_->MaxScroll() > 0 ? static_cast<float>(impl_->scroll_y) / impl_->MaxScroll() : 0.0f;
  impl_->zoom = std::max(kMinZoom, impl_->zoom - kZoomStep);
  impl_->Relayout();
  impl_->scroll_y = std::clamp(static_cast<int>(std::lround(pct * impl_->MaxScroll())), 0, impl_->MaxScroll());
}

void EpubFlowReader::ZoomIn() {
  if (!IsOpen()) return;
  const float pct = impl_->MaxScroll() > 0 ? static_cast<float>(impl_->scroll_y) / impl_->MaxScroll() : 0.0f;
  impl_->zoom = std::min(kMaxZoom, impl_->zoom + kZoomStep);
  impl_->Relayout();
  impl_->scroll_y = std::clamp(static_cast<int>(std::lround(pct * impl_->MaxScroll())), 0, impl_->MaxScroll());
}

void EpubFlowReader::ResetView() {
  if (!IsOpen()) return;
  impl_->zoom = 1.0f;
  impl_->Relayout();
}

void EpubFlowReader::SetBaseFontPointSize(int base_font_pt) {
  if (!impl_) return;
  const int clamped = std::max(8, base_font_pt);
  if (impl_->base_font_pt == clamped) return;
  const float pct = impl_->MaxScroll() > 0 ? static_cast<float>(impl_->scroll_y) / impl_->MaxScroll() : 0.0f;
  impl_->base_font_pt = clamped;
  impl_->Relayout();
  impl_->scroll_y = std::clamp(static_cast<int>(std::lround(pct * impl_->MaxScroll())), 0, impl_->MaxScroll());
}

void EpubFlowReader::SetColors(SDL_Color background_color, SDL_Color font_color) {
  if (!impl_) return;
  const bool font_changed = impl_->font_color.r != font_color.r || impl_->font_color.g != font_color.g ||
                            impl_->font_color.b != font_color.b || impl_->font_color.a != font_color.a;
  impl_->background_color = background_color;
  impl_->font_color = font_color;
  if (font_changed) impl_->DestroyTextTextures();
}

void EpubFlowReader::ScrollByPixels(int delta_px) {
  if (!IsOpen()) return;
  impl_->scroll_y = std::clamp(impl_->scroll_y + delta_px, 0, impl_->MaxScroll());
  impl_->LoadAheadIfNeeded();
}

void EpubFlowReader::JumpByScreen(int direction) {
  if (!IsOpen() || direction == 0) return;
  ScrollByPixels(direction * std::max(1, impl_->ViewHeight() - 24));
}

void EpubFlowReader::SetPage(int page_index) {
  if (!IsOpen()) return;
  while (!impl_->all_docs_loaded &&
         page_index >= (impl_->doc_h + impl_->ViewHeight() - 1) / std::max(1, impl_->ViewHeight())) {
    if (!impl_->LoadMoreDocs(kMoreFlowDocsToLoad)) break;
  }
  const int pages = PageCount();
  const int page = std::clamp(page_index, 0, std::max(0, pages - 1));
  impl_->scroll_y = std::clamp(page * impl_->ViewHeight(), 0, impl_->MaxScroll());
  impl_->LoadAheadIfNeeded();
}

int EpubFlowReader::PageCount() const {
  if (!IsOpen()) return 0;
  const int loaded_pages = std::max(1, (impl_->doc_h + impl_->ViewHeight() - 1) / impl_->ViewHeight());
  if (impl_->all_docs_loaded || impl_->next_doc_index == 0) return loaded_pages;
  const float docs_ratio = static_cast<float>(impl_->ordered_docs.size()) /
                           static_cast<float>(std::max<size_t>(1, impl_->next_doc_index));
  return std::max(loaded_pages, static_cast<int>(std::ceil(loaded_pages * docs_ratio)));
}

bool EpubFlowReader::PageSize(int page_index, int &w, int &h) const {
  (void)page_index;
  if (!IsOpen()) return false;
  w = (impl_->rotation == 90 || impl_->rotation == 270) ? impl_->screen_h : impl_->screen_w;
  h = impl_->ViewHeight();
  return true;
}

int EpubFlowReader::CurrentPage() const {
  if (!IsOpen()) return 0;
  return std::clamp(impl_->scroll_y / std::max(1, impl_->ViewHeight()), 0, std::max(0, PageCount() - 1));
}

EpubRuntimeProgress EpubFlowReader::Progress() const {
  EpubRuntimeProgress out;
  if (!impl_) return out;
  out.page = CurrentPage();
  out.rotation = impl_->rotation;
  out.zoom = impl_->zoom;
  out.scroll_y = impl_->scroll_y;
  return out;
}

bool EpubFlowReader::LooksLikeMixedLayout(const std::string &path) {
#ifndef HAVE_LIBZIP
  (void)path;
  return false;
#else
  int zerr = 0;
  zip_t *za = zip_open(path.c_str(), ZIP_RDONLY, &zerr);
  if (!za) return false;
  ParsedPackage pkg;
  std::string error;
  if (!ParsePackage(za, pkg, error)) {
    zip_close(za);
    runtime_log::Line("[epub_flow] probe package parse failed path=" + path + " error=" + error);
    return false;
  }
  size_t text_chars = 0;
  NaturalTextStats natural_stats;
  size_t image_count = 0;
  size_t docs_read = 0;
  for (const auto &doc : pkg.ordered_docs) {
    std::string html;
    if (!ReadZipEntry(za, doc, html)) continue;
    ++docs_read;
    std::string text = StripTagsToText(html);
    text_chars += text.size();
    NaturalTextStats doc_stats = MeasureNaturalText(text);
    natural_stats.natural_chars += doc_stats.natural_chars;
    natural_stats.natural_blocks += doc_stats.natural_blocks;
    natural_stats.long_natural_blocks += doc_stats.long_natural_blocks;
    natural_stats.max_run = std::max(natural_stats.max_run, doc_stats.max_run);
    image_count += CountImgTagsLinear(html);
    if (natural_stats.natural_chars >= kNaturalTextThreshold &&
        natural_stats.natural_blocks >= kMinNaturalBlocks &&
        natural_stats.long_natural_blocks > 0) {
      break;
    }
    if (docs_read >= 32) break;
  }
  zip_close(za);
  const bool has_natural_text = natural_stats.natural_chars >= kNaturalTextThreshold &&
                                natural_stats.natural_blocks >= kMinNaturalBlocks &&
                                natural_stats.long_natural_blocks > 0;
  const bool image_heavy_noise = image_count >= kImageHeavyThreshold && !has_natural_text;
  const bool flow = has_natural_text && !image_heavy_noise;
  runtime_log::Line("[epub_flow] probe path=" + path + " docs=" + std::to_string(docs_read) +
                    " text=" + std::to_string(text_chars) + " img=" + std::to_string(image_count) +
                    " natural=" + std::to_string(natural_stats.natural_chars) +
                    " natural_blocks=" + std::to_string(natural_stats.natural_blocks) +
                    " long_blocks=" + std::to_string(natural_stats.long_natural_blocks) +
                    " max_run=" + std::to_string(natural_stats.max_run) +
                    " result=" + (flow ? "flow" : "comic"));
  return flow;
#endif
}

bool EpubFlowReader::ExtractFirstDocumentImage(const std::string &path, std::string &bytes, std::string &error) {
#ifndef HAVE_LIBZIP
  (void)path;
  (void)bytes;
  error = "libzip unavailable";
  return false;
#else
  int zerr = 0;
  zip_t *za = zip_open(path.c_str(), ZIP_RDONLY, &zerr);
  if (!za) {
    error = "zip open failed";
    return false;
  }
  ParsedPackage pkg;
  if (!ParsePackage(za, pkg, error)) {
    zip_close(za);
    return false;
  }
  const std::regex img_tag_re("<img\\b([^>]*)>", std::regex::icase);
  for (const auto &doc : pkg.ordered_docs) {
    std::string html;
    if (!ReadZipEntry(za, doc, html)) continue;
    const std::string doc_base = std::filesystem::path(doc).parent_path().generic_string();
    for (std::sregex_iterator it(html.begin(), html.end(), img_tag_re), end; it != end; ++it) {
      AttrMap attrs = ParseTagAttrs((*it)[1].str());
      const auto src_it = attrs.find("src");
      if (src_it == attrs.end() || src_it->second.empty()) continue;
      const std::string img_entry = ResolveRelative(doc_base, src_it->second);
      if (ReadZipEntry(za, img_entry, bytes) && !bytes.empty()) {
        zip_close(za);
        return true;
      }
    }
  }
  zip_close(za);
  error = "no document image found";
  return false;
#endif
}
