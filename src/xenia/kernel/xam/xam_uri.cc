/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_uri.h"

#include <cstdio>
#include <cstring>

#include "xenia/base/string.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

// The four schemes that END a resolution. xam's sub_8199F100 compares against
// exactly these before it looks at either table; whichever one is left after
// the rewrites is what actually gets launched.
constexpr const char* kTerminalSchemes[] = {"title", "app", "game", "sap"};

// The title table, 12 bytes a record at 0x81637700 in the 17559 xam.
//
// `kind` is the record's third word and decides the template:
//   1, 2 - "title:%08X:%s", and ONLY for a caller that is already trusted
//   3    - "app:%08X:%s", no trust needed
// Anything else is 0x80004005.
struct TitleRecord {
  const char* name;
  uint32_t title_id;
  uint32_t kind;
};

constexpr TitleRecord kTitleTable[] = {
    {"dash", 0xFFFE07D1, 1},
    {"avatareditor", 0x584D07D1, 1},
    {"zune", 0x5848085B, 3},
    {"newlivesignup", 0xFFFE07DE, 1},
};

// The scheme rewrite table, 16 bytes a record at 0x81637748.
//
// `allow_untrusted` is the record's second word. A record with 0 refuses a
// caller that has not already been trusted - but matching ANY record makes
// the caller trusted from then on, which is how an untrusted `gameslibrary:`
// legitimately reaches `hub:` (0) two rewrites later.
//
// Two expansions per record. The real selector is an in-process call to app
// 0xFC message 0x00058003: when it succeeds and answers anything but
// 0x001510F1 the SECOND expansion is used, otherwise the first. That is what
// `prefer_content_explorer` carries. The second is always the
// Dash.MP.ContentExplorer.lex flavour and the first the BuiltIn.ContentApp.xzp
// one, which is a package inside the dashboard's own UI - so a console
// without the ContentExplorer module still resolves these tiles.
struct SchemeRecord {
  const char* name;
  uint32_t allow_untrusted;
  const char* expansion;
  const char* content_explorer_expansion;
};

constexpr SchemeRecord kSchemeTable[] = {
    {"pam", 0, "dash:pam:%s", nullptr},
    {"hub", 0, "dash:pam:BuiltIn.Hub.xzp:%s", nullptr},
    {"epix", 0, "hub:root:Epix:%s", nullptr},
    {"gamedetails", 1, "hub:root:BuiltIn.ContentApp.xzp:gamedetails:MediaId=%s",
     "hub:root:Dash.MP.ContentExplorer.lex:gamedetails:MediaId=%s"},
    {"appdetails", 1, "hub:root:BuiltIn.ContentApp.xzp:appdetails:%s",
     "hub:root:Dash.MP.ContentExplorer.lex:appdetails:%s"},
    {"contentdetails", 1, "hub:root:BuiltIn.ContentApp.xzp:contentdetails:%s",
     "hub:root:Dash.MP.ContentExplorer.lex:contentdetails:%s"},
    {"contentlist", 1, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:contentlist:%s"},
    {"contentlibrary", 1, "hub:root:BuiltIn.ContentApp.xzp:contentlibrary:%s",
     "hub:root:Dash.MP.ContentExplorer.lex:contentlibrary:%s"},
    // The My Games tile. PackageType 1 is games and 2 is apps - the same
    // split the dashboard's own DataSet.Library makes.
    {"gameslibrary", 1, "contentlibrary:PackageType=1;%s", nullptr},
    // The My Apps tile.
    {"applibrary", 1, "contentlibrary:PackageType=2;%s", nullptr},
    {"gamesmp", 1, nullptr, "hub:root:Dash.MP.ContentExplorer.lex:gamesmp:%s"},
    {"appsmp", 1, nullptr, "hub:root:Dash.MP.ContentExplorer.lex:appsmp:%s"},
    {"userpins", 1, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:userpins:%s"},
    {"videosmp", 1, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:videosmp:%s"},
    // These seven carry their only expansion in the SECOND slot, so they
    // resolve just on a console that has the ContentExplorer module.
    {"search", 1, nullptr, "pam:Dash.Search.xex:scene=results|%s"},
    {"moviedetails", 1, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=Movie|id=%s"},
    {"tvEpisodedetails", 1, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=TVEpisode|id=%s"},
    {"tvSeasondetails", 1, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=TVSeason|id=%s"},
    {"tvSeriesdetails", 1, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=TVSeries|id=%s"},
    {"tvShowdetails", 1, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=TVShow|id=%s"},
    {"webvideocollection", 1, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=WebVideoCollection|id=%s"},
    {"picturelibrary", 0, "dash:library:picture:%s", nullptr},
    {"videolibrary", 0, "dash:library:video:%s", nullptr},
    {"musiclibrary", 0, "dash:library:music:%s", nullptr},
    // No %s - the remainder is deliberately dropped.
    {"networkhelp", 1, "dash:networktroubleshooter", nullptr},
    {"mediaoverlay", 0, nullptr,
     "dash:pamoverlay:BuiltIn.Hub.xzp:overlay:Dash.MP.ContentExplorer.lex:"
     "overlay:%s"},
    {"http", 1, nullptr, "app:58480880:http:%s"},
    {"https", 1, nullptr, "app:58480880:https:%s"},
    {"microsoftstore", 1, nullptr, "pam:Dash.MP.MicrosoftStore.lex:%s"},
    {"newlivesignup", 0, "app:FFFE07DE:%s", "app:FFFE07DE:%s"},
    {"activityfeed", 0, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:ActivityFeed:%s"},
    {"activitydetails", 0, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:ActivityDetails:%s"},
    {"activityalerts", 0, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:ActivityAlerts:%s"},

    {"nexia", 1, "pam:%s", nullptr},
};

// xam's resolver walks at most ten rewrites before giving up.
constexpr int kMaxRewrites = 10;
// Every buffer on this path is 0x3FC bytes including the terminator.
constexpr size_t kUriCapacity = 0x3FC;

bool EqualsNoCase(const std::string& a, const char* b) {
  return xe_strcasecmp(a.c_str(), b) == 0;
}

// The single "%s" a template carries, replaced by the remainder. A template
// with none drops the remainder, which snprintf does on hardware too.
std::string Substitute(const std::string& format, const std::string& rest) {
  const size_t at = format.find("%s");
  if (at == std::string::npos) {
    return format;
  }
  return format.substr(0, at) + rest + format.substr(at + 2);
}

}  // namespace

X_HRESULT ResolveUri(const std::string& uri, bool trusted,
                     bool prefer_content_explorer, UriResolution* out) {
  if (!out) {
    return X_E_INVALIDARG;
  }
  *out = UriResolution();

  std::string current = uri;
  for (int pass = 0; pass < kMaxRewrites; ++pass) {
    // A URI with no colon is all scheme and no remainder.
    const size_t colon = current.find(':');
    const std::string scheme =
        colon == std::string::npos ? current : current.substr(0, colon);
    const std::string rest =
        colon == std::string::npos ? std::string() : current.substr(colon + 1);
    // The scheme buffer on hardware is 0x21 bytes, terminator included.
    if (scheme.empty() || scheme.size() >= 0x21) {
      return X_E_INVALIDARG;
    }

    for (const char* terminal : kTerminalSchemes) {
      if (!EqualsNoCase(scheme, terminal)) {
        continue;
      }
      out->uri = current;
      out->scheme = scheme;
      out->rest = rest;
      // "%08X:" - eight hex digits and a colon - is what the terminal
      // schemes carry, and it is the title the URI finally names.
      if (rest.size() >= 9 && rest[8] == ':') {
        uint32_t title_id = 0;
        size_t digits = 0;
        for (; digits < 8; ++digits) {
          const char c = rest[digits];
          uint32_t value;
          if (c >= '0' && c <= '9') {
            value = uint32_t(c - '0');
          } else if (c >= 'a' && c <= 'f') {
            value = uint32_t(c - 'a') + 10;
          } else if (c >= 'A' && c <= 'F') {
            value = uint32_t(c - 'A') + 10;
          } else {
            break;
          }
          title_id = (title_id << 4) | value;
        }
        if (digits == 8) {
          out->has_title_id = true;
          out->title_id = title_id;
          out->args = rest.substr(9);
        }
      }
      return X_E_SUCCESS;
    }

    // Not terminal, so it is either a title name or a rewrite. A title name
    // produces the template; anything else goes to the scheme table.
    std::string format;
    const TitleRecord* title = nullptr;
    for (const auto& record : kTitleTable) {
      if (EqualsNoCase(scheme, record.name)) {
        title = &record;
        break;
      }
    }
    if (title) {
      char buffer[64] = {};
      if (title->kind >= 3) {
        if (title->kind != 3) {
          return X_E_FAIL;
        }
        std::snprintf(buffer, sizeof(buffer), "app:%08X:%%s", title->title_id);
      } else {
        if (!trusted) {
          return X_E_ACCESS_DENIED;
        }
        std::snprintf(buffer, sizeof(buffer), "title:%08X:%%s",
                      title->title_id);
      }
      format = buffer;
    } else {
      const SchemeRecord* record = nullptr;
      for (const auto& candidate : kSchemeTable) {
        if (EqualsNoCase(scheme, candidate.name)) {
          record = &candidate;
          break;
        }
      }
      if (!record) {
        // No provider claimed the scheme. sub_8199F270 falls out of its
        // provider list with 0x80070057, which is what ends the whole call.
        return X_E_INVALIDARG;
      }
      if (!trusted && !record->allow_untrusted) {
        return X_E_ACCESS_DENIED;
      }
      // Matching a record is what earns trust for the rest of the chain.
      trusted = true;
      const char* expansion = record->expansion;
      if (prefer_content_explorer && record->content_explorer_expansion) {
        expansion = record->content_explorer_expansion;
      }
      if (!expansion) {
        // The record exists but has nothing for this flavour. The provider
        // answers S_OK without setting its expanded flag, so the walk ends
        // the same way an unknown scheme does.
        return X_E_INVALIDARG;
      }
      format = expansion;
    }

    current = Substitute(format, rest);
    if (current.empty() || current.size() >= kUriCapacity) {
      return X_E_INSUFFICIENT_BUFFER;
    }
  }

  // Ten rewrites and still not a title.
  return X_E_INVALIDARG;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
