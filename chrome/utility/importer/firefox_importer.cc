// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/utility/importer/firefox_importer.h"

#include <set>

#include "base/file_util.h"
#include "base/files/file_enumerator.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop/message_loop.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/importer/firefox_importer_utils.h"
#include "chrome/common/importer/firefox_importer_utils.h"
#include "chrome/common/importer/imported_bookmark_entry.h"
#include "chrome/common/importer/imported_favicon_usage.h"
#include "chrome/common/importer/importer_bridge.h"
#include "chrome/common/importer/importer_url_row.h"
#include "chrome/common/time_format.h"
#include "chrome/utility/importer/bookmark_html_reader.h"
#include "chrome/utility/importer/favicon_reencode.h"
#include "chrome/utility/importer/nss_decryptor.h"
#include "content/public/common/password_form.h"
#include "grit/generated_resources.h"
#include "sql/connection.h"
#include "sql/statement.h"
#include "url/gurl.h"

namespace {

// Original definition is in http://mxr.mozilla.org/firefox/source/toolkit/
//  components/places/public/nsINavBookmarksService.idl
enum BookmarkItemType {
  TYPE_BOOKMARK = 1,
  TYPE_FOLDER = 2,
  TYPE_SEPARATOR = 3,
  TYPE_DYNAMIC_CONTAINER = 4
};

// Loads the default bookmarks in the Firefox installed at |app_path|,
// and stores their locations in |urls|.
void LoadDefaultBookmarks(const base::FilePath& app_path,
                          std::set<GURL>* urls) {
  base::FilePath file = app_path.AppendASCII("defaults")
      .AppendASCII("profile")
      .AppendASCII("bookmarks.html");
  urls->clear();

  std::vector<ImportedBookmarkEntry> bookmarks;
  bookmark_html_reader::ImportBookmarksFile(base::Callback<bool(void)>(),
                                            base::Callback<bool(const GURL&)>(),
                                            file,
                                            &bookmarks,
                                            NULL);
  for (size_t i = 0; i < bookmarks.size(); ++i)
    urls->insert(bookmarks[i].url);
}

// Returns true if |url| has a valid scheme that we allow to import. We
// filter out the URL with a unsupported scheme.
bool CanImportURL(const GURL& url) {
  // The URL is not valid.
  if (!url.is_valid())
    return false;

  // Filter out the URLs with unsupported schemes.
  const char* const kInvalidSchemes[] = {"wyciwyg", "place", "about", "chrome"};
  for (size_t i = 0; i < arraysize(kInvalidSchemes); ++i) {
    if (url.SchemeIs(kInvalidSchemes[i]))
      return false;
  }

  return true;
}

}  // namespace

struct FirefoxImporter::BookmarkItem {
  int parent;
  int id;
  GURL url;
  string16 title;
  BookmarkItemType type;
  std::string keyword;
  base::Time date_added;
  int64 favicon;
  bool empty_folder;
};

FirefoxImporter::FirefoxImporter() {
}

FirefoxImporter::~FirefoxImporter() {
}

void FirefoxImporter::StartImport(
    const importer::SourceProfile& source_profile,
    uint16 items,
    ImporterBridge* bridge) {
  bridge_ = bridge;
  source_path_ = source_profile.source_path;
  app_path_ = source_profile.app_path;

#if defined(OS_POSIX)
  locale_ = source_profile.locale;
#endif

  // The order here is important!
  bridge_->NotifyStarted();
  if ((items & importer::HOME_PAGE) && !cancelled()) {
    bridge_->NotifyItemStarted(importer::HOME_PAGE);
    ImportHomepage();  // Doesn't have a UI item.
    bridge_->NotifyItemEnded(importer::HOME_PAGE);
  }

  // Note history should be imported before bookmarks because bookmark import
  // will also import favicons and we store favicon for a URL only if the URL
  // exist in history or bookmarks.
  if ((items & importer::HISTORY) && !cancelled()) {
    bridge_->NotifyItemStarted(importer::HISTORY);
    ImportHistory();
    bridge_->NotifyItemEnded(importer::HISTORY);
  }

  if ((items & importer::FAVORITES) && !cancelled()) {
    bridge_->NotifyItemStarted(importer::FAVORITES);
    ImportBookmarks();
    bridge_->NotifyItemEnded(importer::FAVORITES);
  }
  if ((items & importer::SEARCH_ENGINES) && !cancelled()) {
    bridge_->NotifyItemStarted(importer::SEARCH_ENGINES);
    ImportSearchEngines();
    bridge_->NotifyItemEnded(importer::SEARCH_ENGINES);
  }
  if ((items & importer::PASSWORDS) && !cancelled()) {
    bridge_->NotifyItemStarted(importer::PASSWORDS);
    ImportPasswords();
    bridge_->NotifyItemEnded(importer::PASSWORDS);
  }
  bridge_->NotifyEnded();
}

void FirefoxImporter::ImportHistory() {
  base::FilePath file = source_path_.AppendASCII("places.sqlite");
  if (!base::PathExists(file))
    return;

  sql::Connection db;
  if (!db.Open(file))
    return;

  // |visit_type| represent the transition type of URLs (typed, click,
  // redirect, bookmark, etc.) We eliminate some URLs like sub-frames and
  // redirects, since we don't want them to appear in history.
  // Firefox transition types are defined in:
  //   toolkit/components/places/public/nsINavHistoryService.idl
  const char* query = "SELECT h.url, h.title, h.visit_count, "
                      "h.hidden, h.typed, v.visit_date "
                      "FROM moz_places h JOIN moz_historyvisits v "
                      "ON h.id = v.place_id "
                      "WHERE v.visit_type <= 3";

  sql::Statement s(db.GetUniqueStatement(query));

  std::vector<ImporterURLRow> rows;
  while (s.Step() && !cancelled()) {
    GURL url(s.ColumnString(0));

    // Filter out unwanted URLs.
    if (!CanImportURL(url))
      continue;

    ImporterURLRow row(url);
    row.title = s.ColumnString16(1);
    row.visit_count = s.ColumnInt(2);
    row.hidden = s.ColumnInt(3) == 1;
    row.typed_count = s.ColumnInt(4);
    row.last_visit = base::Time::FromTimeT(s.ColumnInt64(5)/1000000);

    rows.push_back(row);
  }

  if (!rows.empty() && !cancelled())
    bridge_->SetHistoryItems(rows, importer::VISIT_SOURCE_FIREFOX_IMPORTED);
}

void FirefoxImporter::ImportBookmarks() {
  base::FilePath file = source_path_.AppendASCII("places.sqlite");
  if (!base::PathExists(file))
    return;

  sql::Connection db;
  if (!db.Open(file))
    return;

  // Get the bookmark folders that we are interested in.
  int toolbar_folder_id = -1;
  int menu_folder_id = -1;
  int unsorted_folder_id = -1;
  LoadRootNodeID(&db, &toolbar_folder_id, &menu_folder_id, &unsorted_folder_id);

  // Load livemark IDs.
  std::set<int> livemark_id;
  LoadLivemarkIDs(&db, &livemark_id);

  // Load the default bookmarks.
  std::set<GURL> default_urls;
  LoadDefaultBookmarks(app_path_, &default_urls);

  BookmarkList list;
  GetTopBookmarkFolder(&db, toolbar_folder_id, &list);
  GetTopBookmarkFolder(&db, menu_folder_id, &list);
  GetTopBookmarkFolder(&db, unsorted_folder_id, &list);
  size_t count = list.size();
  for (size_t i = 0; i < count; ++i)
    GetWholeBookmarkFolder(&db, &list, i, NULL);

  std::vector<ImportedBookmarkEntry> bookmarks;
  std::vector<importer::URLKeywordInfo> url_keywords;
  FaviconMap favicon_map;

  // TODO(jcampan): http://b/issue?id=1196285 we do not support POST based
  //                keywords yet.  We won't include them in the list.
  std::set<int> post_keyword_ids;
  const char* query = "SELECT b.id FROM moz_bookmarks b "
      "INNER JOIN moz_items_annos ia ON ia.item_id = b.id "
      "INNER JOIN moz_anno_attributes aa ON ia.anno_attribute_id = aa.id "
      "WHERE aa.name = 'bookmarkProperties/POSTData'";
  sql::Statement s(db.GetUniqueStatement(query));

  if (!s.is_valid())
    return;

  while (s.Step() && !cancelled())
    post_keyword_ids.insert(s.ColumnInt(0));

  for (size_t i = 0; i < list.size(); ++i) {
    BookmarkItem* item = list[i];

    if (item->type == TYPE_FOLDER) {
      // Folders are added implicitly on adding children, so we only explicitly
      // add empty folders.
      if (!item->empty_folder)
        continue;
    } else if (item->type == TYPE_BOOKMARK) {
      // Import only valid bookmarks
      if (!CanImportURL(item->url))
        continue;
    } else {
      continue;
    }

    // Skip the default bookmarks and unwanted URLs.
    if (default_urls.find(item->url) != default_urls.end() ||
        post_keyword_ids.find(item->id) != post_keyword_ids.end())
      continue;

    // Find the bookmark path by tracing their links to parent folders.
    std::vector<string16> path;
    BookmarkItem* child = item;
    bool found_path = false;
    bool is_in_toolbar = false;
    while (child->parent >= 0) {
      BookmarkItem* parent = list[child->parent];
      if (livemark_id.find(parent->id) != livemark_id.end()) {
        // Don't import live bookmarks.
        break;
      }

      if (parent->id != menu_folder_id) {
        // To avoid excessive nesting, omit the name for the bookmarks menu
        // folder.
        path.insert(path.begin(), parent->title);
      }

      if (parent->id == toolbar_folder_id)
        is_in_toolbar = true;

      if (parent->id == toolbar_folder_id ||
          parent->id == menu_folder_id ||
          parent->id == unsorted_folder_id) {
        // We've reached a root node, hooray!
        found_path = true;
        break;
      }

      child = parent;
    }

    if (!found_path)
      continue;

    ImportedBookmarkEntry entry;
    entry.creation_time = item->date_added;
    entry.title = item->title;
    entry.url = item->url;
    entry.path = path;
    entry.in_toolbar = is_in_toolbar;
    entry.is_folder = item->type == TYPE_FOLDER;

    bookmarks.push_back(entry);

    if (item->type == TYPE_BOOKMARK) {
      if (item->favicon)
        favicon_map[item->favicon].insert(item->url);

      // This bookmark has a keyword, we should import it.
      if (!item->keyword.empty() && item->url.is_valid()) {
        importer::URLKeywordInfo url_keyword_info;
        url_keyword_info.url = item->url;
        url_keyword_info.keyword.assign(UTF8ToUTF16(item->keyword));
        url_keyword_info.display_name = item->title;
        url_keywords.push_back(url_keyword_info);
      }
    }
  }

  STLDeleteElements(&list);

  // Write into profile.
  if (!bookmarks.empty() && !cancelled()) {
    const string16& first_folder_name =
        bridge_->GetLocalizedString(IDS_BOOKMARK_GROUP_FROM_FIREFOX);
    bridge_->AddBookmarks(bookmarks, first_folder_name);
  }
  if (!url_keywords.empty() && !cancelled()) {
    bridge_->SetKeywords(url_keywords, false);
  }
  if (!favicon_map.empty() && !cancelled()) {
    std::vector<ImportedFaviconUsage> favicons;
    LoadFavicons(&db, favicon_map, &favicons);
    bridge_->SetFavicons(favicons);
  }
}

void FirefoxImporter::ImportPasswords() {
  // Initializes NSS3.
  NSSDecryptor decryptor;
  if (!decryptor.Init(source_path_, source_path_) &&
      !decryptor.Init(app_path_, source_path_)) {
    return;
  }

  std::vector<content::PasswordForm> forms;
  base::FilePath source_path = source_path_;
  base::FilePath file = source_path.AppendASCII("signons.sqlite");
  if (base::PathExists(file)) {
    // Since Firefox 3.1, passwords are in signons.sqlite db.
    decryptor.ReadAndParseSignons(file, &forms);
  } else {
    // Firefox 3.0 uses signons3.txt to store the passwords.
    file = source_path.AppendASCII("signons3.txt");
    if (!base::PathExists(file))
      file = source_path.AppendASCII("signons2.txt");

    std::string content;
    file_util::ReadFileToString(file, &content);
    decryptor.ParseSignons(content, &forms);
  }

  if (!cancelled()) {
    for (size_t i = 0; i < forms.size(); ++i) {
      bridge_->SetPasswordForm(forms[i]);
    }
  }
}

void FirefoxImporter::ImportSearchEngines() {
  std::vector<std::string> search_engine_data;
  GetSearchEnginesXMLData(&search_engine_data);

  bridge_->SetFirefoxSearchEnginesXMLData(search_engine_data);
}

void FirefoxImporter::ImportHomepage() {
  GURL home_page = GetHomepage(source_path_);
  if (home_page.is_valid() && !IsDefaultHomepage(home_page, app_path_)) {
    bridge_->AddHomePage(home_page);
  }
}

void FirefoxImporter::GetSearchEnginesXMLData(
    std::vector<std::string>* search_engine_data) {
  base::FilePath file = source_path_.AppendASCII("search.sqlite");
  if (!base::PathExists(file))
    return;

  sql::Connection db;
  if (!db.Open(file))
    return;

  const char* query = "SELECT engineid FROM engine_data "
                      "WHERE engineid NOT IN "
                      "(SELECT engineid FROM engine_data "
                      "WHERE name='hidden') "
                      "ORDER BY value ASC";

  sql::Statement s(db.GetUniqueStatement(query));
  if (!s.is_valid())
    return;

  base::FilePath app_path = app_path_.AppendASCII("searchplugins");
  base::FilePath profile_path = source_path_.AppendASCII("searchplugins");

  // Firefox doesn't store a search engine in its sqlite database unless the
  // user has added a engine. So we get search engines from sqlite db as well
  // as from the file system.
  if (s.Step()) {
    const std::string kAppPrefix("[app]/");
    const std::string kProfilePrefix("[profile]/");
    do {
      base::FilePath file;
      std::string engine(s.ColumnString(0));

      // The string contains [app]/<name>.xml or [profile]/<name>.xml where
      // the [app] and [profile] need to be replaced with the actual app or
      // profile path.
      size_t index = engine.find(kAppPrefix);
      if (index != std::string::npos) {
        // Remove '[app]/'.
        file = app_path.AppendASCII(engine.substr(index + kAppPrefix.length()));
      } else if ((index = engine.find(kProfilePrefix)) != std::string::npos) {
        // Remove '[profile]/'.
          file = profile_path.AppendASCII(
              engine.substr(index + kProfilePrefix.length()));
      } else {
        // Looks like absolute path to the file.
        file = base::FilePath::FromUTF8Unsafe(engine);
      }
      std::string file_data;
      file_util::ReadFileToString(file, &file_data);
      search_engine_data->push_back(file_data);
    } while (s.Step() && !cancelled());
  }

#if defined(OS_POSIX)
  // Ubuntu-flavored Firefox supports locale-specific search engines via
  // locale-named subdirectories. They fall back to en-US.
  // See http://crbug.com/53899
  // TODO(jshin): we need to make sure our locale code matches that of
  // Firefox.
  DCHECK(!locale_.empty());
  base::FilePath locale_app_path = app_path.AppendASCII(locale_);
  base::FilePath default_locale_app_path = app_path.AppendASCII("en-US");
  if (base::DirectoryExists(locale_app_path))
    app_path = locale_app_path;
  else if (base::DirectoryExists(default_locale_app_path))
    app_path = default_locale_app_path;
#endif

  // Get search engine definition from file system.
  base::FileEnumerator engines(app_path, false, base::FileEnumerator::FILES);
  for (base::FilePath engine_path = engines.Next();
       !engine_path.value().empty(); engine_path = engines.Next()) {
    std::string file_data;
    file_util::ReadFileToString(file, &file_data);
    search_engine_data->push_back(file_data);
  }
}

void FirefoxImporter::LoadRootNodeID(sql::Connection* db,
                                      int* toolbar_folder_id,
                                      int* menu_folder_id,
                                      int* unsorted_folder_id) {
  static const char* kToolbarFolderName = "toolbar";
  static const char* kMenuFolderName = "menu";
  static const char* kUnsortedFolderName = "unfiled";

  const char* query = "SELECT root_name, folder_id FROM moz_bookmarks_roots";
  sql::Statement s(db->GetUniqueStatement(query));

  while (s.Step()) {
    std::string folder = s.ColumnString(0);
    int id = s.ColumnInt(1);
    if (folder == kToolbarFolderName)
      *toolbar_folder_id = id;
    else if (folder == kMenuFolderName)
      *menu_folder_id = id;
    else if (folder == kUnsortedFolderName)
      *unsorted_folder_id = id;
  }
}

void FirefoxImporter::LoadLivemarkIDs(sql::Connection* db,
                                       std::set<int>* livemark) {
  static const char* kFeedAnnotation = "livemark/feedURI";
  livemark->clear();

  const char* query = "SELECT b.item_id "
                      "FROM moz_anno_attributes a "
                      "JOIN moz_items_annos b ON a.id = b.anno_attribute_id "
                      "WHERE a.name = ? ";
  sql::Statement s(db->GetUniqueStatement(query));
  s.BindString(0, kFeedAnnotation);

  while (s.Step() && !cancelled())
    livemark->insert(s.ColumnInt(0));
}

void FirefoxImporter::GetTopBookmarkFolder(sql::Connection* db,
                                            int folder_id,
                                            BookmarkList* list) {
  const char* query = "SELECT b.title "
                     "FROM moz_bookmarks b "
                     "WHERE b.type = 2 AND b.id = ? "
                     "ORDER BY b.position";
  sql::Statement s(db->GetUniqueStatement(query));
  s.BindInt(0, folder_id);

  if (s.Step()) {
    BookmarkItem* item = new BookmarkItem;
    item->parent = -1;  // The top level folder has no parent.
    item->id = folder_id;
    item->title = s.ColumnString16(0);
    item->type = TYPE_FOLDER;
    item->favicon = 0;
    item->empty_folder = true;
    list->push_back(item);
  }
}

void FirefoxImporter::GetWholeBookmarkFolder(sql::Connection* db,
                                              BookmarkList* list,
                                              size_t position,
                                              bool* empty_folder) {
  if (position >= list->size()) {
    NOTREACHED();
    return;
  }

  const char* query = "SELECT b.id, h.url, COALESCE(b.title, h.title), "
         "b.type, k.keyword, b.dateAdded, h.favicon_id "
         "FROM moz_bookmarks b "
         "LEFT JOIN moz_places h ON b.fk = h.id "
         "LEFT JOIN moz_keywords k ON k.id = b.keyword_id "
         "WHERE b.type IN (1,2) AND b.parent = ? "
         "ORDER BY b.position";
  sql::Statement s(db->GetUniqueStatement(query));
  s.BindInt(0, (*list)[position]->id);

  BookmarkList temp_list;
  while (s.Step()) {
    BookmarkItem* item = new BookmarkItem;
    item->parent = static_cast<int>(position);
    item->id = s.ColumnInt(0);
    item->url = GURL(s.ColumnString(1));
    item->title = s.ColumnString16(2);
    item->type = static_cast<BookmarkItemType>(s.ColumnInt(3));
    item->keyword = s.ColumnString(4);
    item->date_added = base::Time::FromTimeT(s.ColumnInt64(5)/1000000);
    item->favicon = s.ColumnInt64(6);
    item->empty_folder = true;

    temp_list.push_back(item);
    if (empty_folder != NULL)
      *empty_folder = false;
  }

  // Appends all items to the list.
  for (BookmarkList::iterator i = temp_list.begin();
       i != temp_list.end(); ++i) {
    list->push_back(*i);
    // Recursive add bookmarks in sub-folders.
    if ((*i)->type == TYPE_FOLDER)
      GetWholeBookmarkFolder(db, list, list->size() - 1, &(*i)->empty_folder);
  }
}

void FirefoxImporter::LoadFavicons(
    sql::Connection* db,
    const FaviconMap& favicon_map,
    std::vector<ImportedFaviconUsage>* favicons) {
  const char* query = "SELECT url, data FROM moz_favicons WHERE id=?";
  sql::Statement s(db->GetUniqueStatement(query));

  if (!s.is_valid())
    return;

  for (FaviconMap::const_iterator i = favicon_map.begin();
       i != favicon_map.end(); ++i) {
    s.BindInt64(0, i->first);
    if (s.Step()) {
      ImportedFaviconUsage usage;

      usage.favicon_url = GURL(s.ColumnString(0));
      if (!usage.favicon_url.is_valid())
        continue;  // Don't bother importing favicons with invalid URLs.

      std::vector<unsigned char> data;
      s.ColumnBlobAsVector(1, &data);
      if (data.empty())
        continue;  // Data definitely invalid.

      if (!importer::ReencodeFavicon(&data[0], data.size(), &usage.png_data))
        continue;  // Unable to decode.

      usage.urls = i->second;
      favicons->push_back(usage);
    }
    s.Reset(true);
  }
}
