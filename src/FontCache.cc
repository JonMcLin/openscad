/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2011 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "FontCache.h"

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <hb.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TYPES_H
#include FT_TRUETYPE_IDS_H

#include "platform/PlatformUtils.h"
#include "utils/printutils.h"
#include "utils/version_helper.h"

extern std::vector<std::string> librarypath;
std::vector<std::string> fontpath;

namespace fs = std::filesystem;

std::string get_fontconfig_version()
{
  const unsigned int version = FcGetVersion();

  const OpenSCAD::library_version_number header_version{FC_MAJOR, FC_MINOR, FC_REVISION};
  const OpenSCAD::library_version_number runtime_version{version / 10000, (version / 100) % 100, version % 100};
  return OpenSCAD::get_version_string(header_version, runtime_version);
}

std::string get_harfbuzz_version()
{
  unsigned int major, minor, micro;
  hb_version(&major, &minor, &micro);

  const OpenSCAD::library_version_number header_version{HB_VERSION_MAJOR, HB_VERSION_MINOR, HB_VERSION_MICRO};
  const OpenSCAD::library_version_number runtime_version{major, minor, micro};
  return OpenSCAD::get_version_string(header_version, runtime_version);
}

std::string get_freetype_version()
{
  return FontCache::instance()->get_freetype_version();
}

FontInfo::FontInfo(std::string family, std::string style, std::string file, uint32_t hash)
   : family(std::move(family)), style(std::move(style)), file(std::move(file)), hash(hash)
{
}

bool FontInfo::operator<(const FontInfo& rhs) const
{
  if (family < rhs.family) {
    return true;
  }
  if (style < rhs.style) {
    return true;
  }
  return file < rhs.file;
}

const std::string& FontInfo::get_family() const
{
  return family;
}

const std::string& FontInfo::get_style() const
{
  return style;
}

const std::string& FontInfo::get_file() const
{
  return file;
}

const uint32_t FontInfo::get_hash() const
{
  return hash;
}

FontCache *FontCache::self = nullptr;
FontCache::InitHandlerFunc *FontCache::cb_handler = FontCache::defaultInitHandler;
void *FontCache::cb_userdata = nullptr;
const std::string FontCache::DEFAULT_FONT("Liberation Sans:style=Regular");

/**
 * Default implementation for the font cache initialization. In case no other
 * handler is registered, the cache build is just called synchronously in the
 * current thread by this handler.
 */
void FontCache::defaultInitHandler(FontCacheInitializer *initializer, void *)
{
  initializer->run();
}

FontCache::FontCache()
{
  this->init_ok = false;
  this->library = nullptr;

  // If we've got a bundled fonts.conf, initialize fontconfig with our own config
  // by overriding the built-in fontconfig path.
  // For system installs and dev environments, we leave this alone
  const fs::path fontdir(PlatformUtils::resourcePath("fonts"));
  if (fs::is_regular_file(fontdir / "fonts.conf")) {
    auto abspath = fontdir.empty() ? fs::current_path() : fs::absolute(fontdir);
    PlatformUtils::setenv("FONTCONFIG_PATH", (abspath.generic_string()).c_str(), 0);
  }

  // Just load the configs. We'll build the fonts once all configs are loaded
  this->config = FcInitLoadConfig();
  if (!this->config) {
    LOG(message_group::Font_Warning, "Can't initialize fontconfig library, text() objects will not be rendered");
    return;
  }

  // Add the built-in fonts & config
  fs::path builtinfontpath(PlatformUtils::resourcePath("fonts"));
  if (fs::is_directory(builtinfontpath)) {
#ifndef __EMSCRIPTEN__
    builtinfontpath = fs::canonical(builtinfontpath);
#endif
    FcConfigParseAndLoad(this->config, reinterpret_cast<const FcChar8 *>(builtinfontpath.generic_string().c_str()), false);
    add_font_dir(builtinfontpath.generic_string());
  }

  const char *home = getenv("HOME");

  // Add Linux font folders, the system folders are expected to be
  // configured by the system configuration for fontconfig.
  if (home) {
    add_font_dir(std::string(home) + "/.fonts");
  }

  const char *env_font_path = getenv("OPENSCAD_FONT_PATH");
  if (env_font_path != nullptr) {
    std::string paths(env_font_path);
    const std::string sep = PlatformUtils::pathSeparatorChar();
    using string_split_iterator = boost::split_iterator<std::string::iterator>;
    for (string_split_iterator it = boost::make_split_iterator(paths, boost::first_finder(sep, boost::is_iequal())); it != string_split_iterator(); ++it) {
      const fs::path p(boost::copy_range<std::string>(*it));
      if (fs::exists(p) && fs::is_directory(p)) {
        const std::string path = fs::absolute(p).string();
        add_font_dir(path);
      }
    }
  }

  FontCacheInitializer initializer(this->config);
  cb_handler(&initializer, cb_userdata);

  // For use by LibraryInfo
  FcStrList *dirs = FcConfigGetFontDirs(this->config);
  while (FcChar8 *dir = FcStrListNext(dirs)) {
    fontpath.emplace_back((const char *)dir);
  }
  FcStrListDone(dirs);

  const FT_Error error = FT_Init_FreeType(&this->library);
  if (error) {
    LOG(message_group::Font_Warning, "Can't initialize freetype library, text() objects will not be rendered");
    return;
  }

  this->init_ok = true;
}

FontCache *FontCache::instance()
{
  if (!self) {
    self = new FontCache();
  }
  return self;
}

const std::string FontCache::get_freetype_version() const
{
  if (!this->is_init_ok()) {
    return "(not initialized)";
  }

  FT_Int major, minor, micro;
  FT_Library_Version(this->library, &major, &minor, &micro);

  const OpenSCAD::library_version_number header_version{FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH};
  const OpenSCAD::library_version_number runtime_version{static_cast<unsigned>(major), static_cast<unsigned>(minor), static_cast<unsigned>(micro)};
  return OpenSCAD::get_version_string(header_version, runtime_version);
}

void FontCache::registerProgressHandler(InitHandlerFunc *handler, void *userdata)
{
  FontCache::cb_handler = handler;
  FontCache::cb_userdata = userdata;
}

void FontCache::register_font_file(const std::string& path)
{
  if (!FcConfigAppFontAddFile(this->config, reinterpret_cast<const FcChar8 *>(path.c_str()))) {
    LOG("Can't register font '%1$s'", path);
  }
}

void FontCache::add_font_dir(const std::string& path)
{
  if (!fs::is_directory(path)) {
    return;
  }
  if (!FcConfigAppFontAddDir(this->config, reinterpret_cast<const FcChar8 *>(path.c_str()))) {
    LOG("Can't register font directory '%1$s'", path);
  }
}

std::vector<uint32_t> FontCache::filter(const std::u32string& str) const
{
  FcObjectSet *object_set = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, nullptr);
  FcPattern *pattern = FcPatternCreate();
  init_pattern(pattern);
  FcCharSet *charSet = FcCharSetCreate();
  for (const char32_t a : str) {
    FcCharSetAddChar(charSet, a);
  }
  FcValue charSetValue;
  charSetValue.type = FcTypeCharSet;
  charSetValue.u.c = charSet;
  FcPatternAdd(pattern, FC_CHARSET, charSetValue, true);

  FcFontSet *font_set = FcFontList(this->config, pattern, object_set);
  FcObjectSetDestroy(object_set);
  FcPatternDestroy(pattern);
  FcCharSetDestroy(charSet);

  std::vector<uint32_t> result;
  result.reserve(font_set->nfont);
  for (int a = 0;a < font_set->nfont;++a) {
    result.push_back(FcPatternHash(font_set->fonts[a]));
  }
  FcFontSetDestroy(font_set);
  return result;
}

FontInfoList *FontCache::list_fonts() const
{
  FcObjectSet *object_set = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, nullptr);
  FcPattern *pattern = FcPatternCreate();
  init_pattern(pattern);
  FcFontSet *font_set = FcFontList(this->config, pattern, object_set);
  FcObjectSetDestroy(object_set);
  FcPatternDestroy(pattern);

  auto *list = new FontInfoList();
  for (int a = 0; a < font_set->nfont; ++a) {
    FcPattern *p = font_set->fonts[a];

    FcChar8 *file_value;
    if (FcPatternGetString(p, FC_FILE, 0, &file_value) != FcResultMatch) {
        continue;
    }

    FcChar8 *family_value;
    if (FcPatternGetString(p, FC_FAMILY, 0, &family_value) != FcResultMatch) {
        continue;
    }

    FcChar8 *style_value;
    if (FcPatternGetString(p, FC_STYLE, 0, &style_value) != FcResultMatch) {
        continue;
    }

    const std::string family((const char *) family_value);
    const std::string style((const char *) style_value);
    const std::string file((const char *) file_value);

    list->emplace_back(family, style, file, FcPatternHash(p));
  }
  FcFontSetDestroy(font_set);

  return list;
}

bool FontCache::is_init_ok() const
{
  return this->init_ok;
}

void FontCache::clear()
{
  this->cache.clear();
}

void FontCache::dump_cache(const std::string& info)
{
  std::cout << info << ":";
  for (const auto& item : this->cache) {
    std::cout << " " << item.first << " (" << item.second.second << ")";
  }
  std::cout << std::endl;
}

void FontCache::check_cleanup()
{
  if (this->cache.size() < MAX_NR_OF_CACHE_ENTRIES) {
    return;
  }

  auto pos = this->cache.begin()++;
  for (auto it = this->cache.begin(); it != this->cache.end(); ++it) {
    if ((*pos).second.second > (*it).second.second) {
      pos = it;
    }
  }
  FontFacePtr face = (*pos).second.first;
  this->cache.erase(pos);
}

FontFacePtr FontCache::get_font(const std::string& font)
{
  FontFacePtr face;
  auto it = this->cache.find(font);
  if (it == this->cache.end()) {
    face = find_face(font);
    if (!face) {
      return nullptr;
    }
    check_cleanup();
  } else {
    face = (*it).second.first;
  }
  this->cache[font] = cache_entry_t(face, time(nullptr));
  return face;
}

FontFacePtr FontCache::find_face(const std::string& font) const
{
  std::string trimmed(font);
  boost::algorithm::trim(trimmed);

  const std::string lookup = trimmed.empty() ? DEFAULT_FONT : trimmed;
  PRINTDB("font = \"%s\", lookup = \"%s\"", font % lookup);
  FontFacePtr face = find_face_fontconfig(lookup);
  if (face) {
    PRINTDB("result = \"%s\", style = \"%s\"", face->face_->family_name % face->face_->style_name);
  } else {
    PRINTD("font not found");
  }
  return face;
}

void FontCache::init_pattern(FcPattern *pattern) const
{
  assert(pattern);
  const FcValue true_value = {
    .type = FcTypeBool,
    .u = {.b = true},
  };

  FcPatternAdd(pattern, FC_OUTLINE, true_value, true);
  FcPatternAdd(pattern, FC_SCALABLE, true_value, true);
}

FontFacePtr FontCache::find_face_fontconfig(const std::string& font) const
{
  FcResult result;

  FcPattern *pattern = FcNameParse((unsigned char *)font.c_str());
  if (!pattern) {
    LOG(message_group::Font_Warning, "Could not parse font '%1$s'", font);
    return nullptr;
  }
  init_pattern(pattern);

  FcConfigSubstitute(this->config, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);

  FcPattern *match = FcFontMatch(this->config, pattern, &result);

  FcChar8 *file_value;
  if (FcPatternGetString(match, FC_FILE, 0, &file_value) != FcResultMatch) {
    return nullptr;
  }

  int font_index;
  if (FcPatternGetInteger(match, FC_INDEX, 0, &font_index) != FcResultMatch) {
    return nullptr;
  }

  FcChar8 *font_features;
  std::string font_features_str;
  if (FcPatternGetString(match, FC_FONT_FEATURES, 0, &font_features) == FcResultMatch) {
      font_features_str = (const char *)(font_features);
      PRINTDB("Found font features: '%s'", font_features_str);
  }

  FT_Face ftFace;
  const FT_Error error = FT_New_Face(this->library, (const char *)file_value, font_index, &ftFace);

  FcPatternDestroy(pattern);
  FcPatternDestroy(match);

  if (error) {
    return nullptr;
  }

  std::vector<std::string> features;
  boost::split(features, font_features_str, boost::is_any_of(";"));
  FontFacePtr face = std::make_shared<const FontFace>(ftFace, features);

  for (int a = 0; a < face->face_->num_charmaps; ++a) {
    FT_CharMap charmap = face->face_->charmaps[a];
    PRINTDB("charmap = %d: platform = %d, encoding = %d", a % charmap->platform_id % charmap->encoding_id);
  }

  if (FT_Select_Charmap(face->face_, ft_encoding_unicode) == 0) {
    PRINTDB("Successfully selected unicode charmap: %s/%s", face->face_->family_name % face->face_->style_name);
  } else {
    bool charmap_set = false;
    if (!charmap_set) charmap_set = try_charmap(face, TT_PLATFORM_MICROSOFT, TT_MS_ID_UNICODE_CS);
    if (!charmap_set) charmap_set = try_charmap(face, TT_PLATFORM_ISO, TT_ISO_ID_10646);
    if (!charmap_set) charmap_set = try_charmap(face, TT_PLATFORM_APPLE_UNICODE, -1);
    if (!charmap_set) charmap_set = try_charmap(face, TT_PLATFORM_MICROSOFT, TT_MS_ID_SYMBOL_CS);
    if (!charmap_set) charmap_set = try_charmap(face, TT_PLATFORM_MACINTOSH, TT_MAC_ID_ROMAN);
    if (!charmap_set) charmap_set = try_charmap(face, TT_PLATFORM_ISO, TT_ISO_ID_8859_1);
    if (!charmap_set) charmap_set = try_charmap(face, TT_PLATFORM_ISO, TT_ISO_ID_7BIT_ASCII);
    if (!charmap_set) LOG(message_group::Font_Warning, "Could not select a char map for font '%1$s/%2$s'", face->face_->family_name, face->face_->style_name);
  }

  return face;
}

bool FontCache::try_charmap(const FontFacePtr& face_ptr, int platform_id, int encoding_id) const
{
  FT_Face face = face_ptr->face_;
  for (int idx = 0; idx < face->num_charmaps; ++idx) {
    FT_CharMap charmap = face->charmaps[idx];
    if ((charmap->platform_id == platform_id) && ((encoding_id < 0) || (charmap->encoding_id == encoding_id))) {
      if (FT_Set_Charmap(face, charmap) == 0) {
        PRINTDB("Selected charmap: platform_id = %d, encoding_id = %d", charmap->platform_id % charmap->encoding_id);
        if (is_windows_symbol_font(face)) {
          PRINTDB("Detected windows symbol font with character codes in the Private Use Area of Unicode at 0xf000: %s/%s", face->family_name % face->style_name);
        }
        return true;
      }
    }
  }
  return false;
}

bool FontCache::is_windows_symbol_font(const FT_Face& face) const
{
  if (face->charmap->platform_id != TT_PLATFORM_MICROSOFT) {
    return false;
  }

  if (face->charmap->encoding_id != TT_MS_ID_SYMBOL_CS) {
    return false;
  }

  FT_UInt gindex;
  const FT_ULong charcode = FT_Get_First_Char(face, &gindex);
  if ((gindex == 0) || (charcode < 0xf000)) {
    return false;
  }

  return true;
}
