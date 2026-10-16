#include "Online/OnlineSource.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <regex>

namespace sw {
namespace {

// `a.b.c` through nested objects. nullptr when any step is missing or is not an object. Arrays are
// not walked by index on purpose: every reply this is used against hands back named members, and a
// rule nobody has to satisfy is a rule that will quietly rot.
const Json* Walk(const Json& root, const std::string& path) {
    const Json* node = &root;
    size_t at = 0;
    while (at < path.size()) {
        const size_t dot = path.find('.', at);
        const std::string key = path.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
        if (!node->isObject()) return nullptr;
        node = node->find(key);
        if (!node) return nullptr;
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
    return node;
}

std::string Text(const Json& item, const std::string& path) {
    const Json* j = Walk(item, path);
    if (!j) return {};
    if (j->isString()) return j->asString();
    if (j->isNumber()) {
        const double d = j->asNumber();
        // 1920 must print as 1920, not 1920.000000: these numbers land on a card.
        if (d == double(long long(d))) return std::to_string(long long(d));
        return std::format("{:.2f}", d);
    }
    return {};
}

long long Number(const Json& item, const std::string& path, long long def = 0) {
    const Json* j = Walk(item, path);
    return j ? (j->isNumber() ? long long(j->asNumber()) : def) : def;
}

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

// What a file name's suffix means to this app. The window reads the mime off the card to decide two
// things - whether 入库 can take it at all, and whether the hero is worth fetching - so a clip called
// "image/jpeg" makes it pull the whole video to preview a card (measured 2026-10-14 on motionbgs:
// 19.4-28.8 MB per selection). nullptr for a suffix nothing here can import, which the card reports
// instead of guessing at a picture.
const char* MimeForExtension(const std::string& ext) {
    static const std::pair<const char*, const char*> table[] = {
        {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"}, {"png", "image/png"},
        {"webp", "image/webp"}, {"bmp", "image/bmp"},   {"tif", "image/tiff"},
        {"tiff", "image/tiff"}, {"gif", "image/gif"},   {"mp4", "video/mp4"},
        {"m4v", "video/mp4"},   {"webm", "video/webm"}, {"mkv", "video/x-matroska"},
    };
    for (const auto& [suffix, mime] : table)
        if (ext == suffix) return mime;
    return nullptr;
}

} // namespace

bool OnlineSource::setOption(const std::string& key, const std::string& value) {
    for (auto& o : options_)
        if (o.key == key) {
            if (std::find(o.values.begin(), o.values.end(), value) == o.values.end()) return false;
            o.current = value;
            return true;
        }
    return false;
}

std::string OnlineSource::optionValue(const std::string& key) const {
    for (const auto& o : options_)
        if (o.key == key) return o.current;
    return {};
}

std::string OnlineSource::Expand(const std::string& value, const std::string& userQuery, int page) const {
    std::string out = ReplaceAll(value, "{query}", userQuery);
    // `{nextpage}` is empty on the first page: a site that lists its pages as ?page=2, ?page=3 answers
    // ?page=1 with a 301 back to the plain url (measured on 4kwallpapers), and a parameter that is
    // dropped because it is empty is exactly what BuildSearch already knows how to do.
    out = ReplaceAll(out, "{nextpage}", page <= 1 ? std::string() : std::to_string(page));
    out = ReplaceAll(out, "{page}", std::to_string(page));
    for (const auto& o : options_)
        out = ReplaceAll(out, "{" + o.key + "}", o.current);
    return out;
}

std::string OnlineSource::ExpandItem(const std::string& value,
                                     const std::vector<std::pair<std::string, std::string>>& vars) const {
    std::string out = ReplaceAll(value, "{homepage}", homepage_);
    for (const auto& o : options_)
        out = ReplaceAll(out, "{" + o.key + "}", o.current);
    for (const auto& [k, v] : vars)
        out = ReplaceAll(out, "{" + k + "}", v);
    return out;
}

std::string OnlineSource::FieldOf(const char* name) const {
    for (const auto& [k, path] : fields_)
        if (k == name) return path;
    return {};
}

bool OnlineSource::ParseHtml(const std::string& body, int askedPage, std::vector<OnlineItem>& items,
                             OnlineMeta& meta, std::string& error) const {
    std::regex rec, last;
    try {
        rec.assign(recordRe_, std::regex_constants::optimize);
        if (!lastPageRe_.empty()) last.assign(lastPageRe_, std::regex_constants::optimize);
    } catch (const std::regex_error& e) {
        error = std::format("{}: 'record' is not a usable expression ({})", name_, e.what());
        return false;
    }
    if (FieldOf("key").empty() || FieldOf("key")[0] != '$') {
        error = std::format("{}: an html source must name 'key' as a group, like \"$3\"", name_);
        return false;
    }
    const int keyGroup = std::stoi(FieldOf("key").substr(1));

    auto group = [](const std::smatch& m, const std::string& ref) -> std::string {
        if (ref.empty() || ref[0] != '$') return {};
        const int n = std::stoi(ref.substr(1));
        return n < int(m.size()) && m[n].matched ? m[n].str() : std::string();
    };

    auto first = std::sregex_iterator(body.begin(), body.end(), rec);
    const auto end = std::sregex_iterator();
    // A field written as "$2" is that capture group; anything else is a template the record's own
    // fields, the options and the homepage get filled into.
    auto itemField = [&](const std::smatch& m,
                         const std::vector<std::pair<std::string, std::string>>& vars,
                         const char* name) {
        const std::string f = FieldOf(name);
        if (f.empty()) return std::string();
        return f[0] == '$' ? group(m, f) : ExpandItem(f, vars);
    };
    for (auto it = first; it != end; ++it) {
        const std::smatch& m = *it;
        std::vector<std::pair<std::string, std::string>> vars;
        for (const auto& [name, ref] : fields_)
            if (!ref.empty() && ref[0] == '$') vars.emplace_back(name, group(m, ref));
        // The slug is the file name of the page url with its id taken back off, which is what the
        // picture urls are built from. Two named steps rather than a second regex, so the description
        // says what it means.
        if (!slugFrom_.empty()) {
            std::string slug = ExpandItem(slugFrom_, vars);
            if (slugBasename_) {
                const size_t slash = slug.rfind('/');
                if (slash != std::string::npos) slug = slug.substr(slash + 1);
            }
            const std::string cut = ExpandItem(slugCut_, vars);
            if (!cut.empty() && slug.size() >= cut.size() && slug.compare(slug.size() - cut.size(), cut.size(), cut) == 0)
                slug.erase(slug.size() - cut.size());
            vars.emplace_back("slug", slug);
        }
        OnlineItem o;
        o.key = keyGroup < int(m.size()) ? m[keyGroup].str() : std::string();
        if (o.key.empty()) continue;
        o.title = itemField(m, vars, "title");
        if (o.title.empty()) o.title = o.key;
        o.page = itemField(m, vars, "page");
        // thumb and image are built, not read: this site's only thumbnails live under a directory its
        // robots.txt disallows, while the pictures themselves are at a url whose size is a word in the
        // path - so the record's own slug and id are enough to name any size we are allowed to take.
        o.thumb = itemField(m, vars, "thumb");
        o.image = itemField(m, vars, "image");
        o.hero = itemField(m, vars, "hero");
        if (o.image.empty()) continue;
        if (o.thumb.empty()) o.thumb = o.image;
        if (!sizeFrom_.empty()) {
            const std::string s = ExpandItem(sizeFrom_, vars);
            const size_t x = s.find('x');
            if (x != std::string::npos) {
                o.width = std::atoi(s.c_str());
                o.height = std::atoi(s.c_str() + x + 1);
            }
        }
        // Named off the file this record points at. The old line could only ever say jpg or png, which
        // is what made a clip source's card claim to be a picture.
        if (const char* mime = MimeForExtension(ExtensionFor(o.image))) o.mime = mime;
        items.push_back(std::move(o));
    }

    meta.page = askedPage;
    meta.lastPage = askedPage;
    if (!lastPageRe_.empty())
        for (auto it = std::sregex_iterator(body.begin(), body.end(), last);
             it != end; ++it)
            meta.lastPage = std::max(meta.lastPage, std::atoi((*it)[1].str().c_str()));
    // The site says how many pages there are and never how many pictures; counting the ones on this
    // page would be a number that changes every page, so the total stays unknown.
    meta.total = 0;
    // A site that links only its neighbour page can never say how many there are; `lastPage` above is
    // then just "there is one more", which is enough for the arrows and not enough for a number.
    meta.pagesKnown = !rollingPages_;
    if (items.empty()) error = "the site returned no picture on this page";
    return !items.empty();
}

OnlineSource::Request OnlineSource::BuildSearch(const std::string& userQuery, int page) const {
    Request r;
    // The endpoint is a template like every parameter is. wallhaven's has no placeholder in it, which
    // is why this only shows up with a site whose category and page live in the path.
    r.url = Expand(searchUrl_, userQuery, page);
    for (const auto& [name, tmpl] : params_) {
        const std::string v = Expand(tmpl, userQuery, page);
        // An empty value is "not asked for", not "match on nothing" - and sites differ between the
        // two, which is the kind of difference that shows up as an empty grid and no error.
        if (v.empty()) continue;
        r.query.emplace_back(name, v);
    }
    return r;
}

bool OnlineSource::ParseSearch(const Json& root, std::vector<OnlineItem>& items, OnlineMeta& meta,
                               std::string& error) const {
    const Json* list = root.find(listPath_);
    if (!list || !list->isArray()) {
        error = std::format("the reply has no list under '{}'", listPath_);
        return false;
    }
    const auto field = [this](const char* name) -> std::string {
        for (const auto& [k, path] : fields_)
            if (k == name) return path;
        return {};
    };
    const std::string fKey = field("key"), fTitle = field("title"), fThumb = field("thumb"),
                      fImage = field("image"), fHero = field("hero"), fPage = field("page"),
                      fW = field("width"), fH = field("height"), fBytes = field("bytes"),
                      fMime = field("mime");
    if (fKey.empty() || fImage.empty()) {
        error = std::format("{} names no 'key' and 'image' field", name_);
        return false;
    }
    for (const Json& it : list->items()) {
        if (!it.isObject()) continue;
        OnlineItem o;
        o.key = Text(it, fKey);
        o.image = Text(it, fImage);
        if (o.key.empty() || o.image.empty()) continue;   // not a wallpaper, whatever it is
        o.title = fTitle.empty() ? o.key : Text(it, fTitle);
        o.thumb = fThumb.empty() ? o.image : Text(it, fThumb);
        if (o.thumb.empty()) o.thumb = o.image;
        o.hero = fHero.empty() ? std::string() : Text(it, fHero);
        o.page = fPage.empty() ? std::string() : Text(it, fPage);
        o.width = int(Number(it, fW));
        o.height = int(Number(it, fH));
        o.bytes = fBytes.empty() ? 0 : Number(it, fBytes);
        o.mime = fMime.empty() ? std::string() : Text(it, fMime);
        items.push_back(std::move(o));
    }
    meta.page = int(Number(root, pagePath_, 1));
    meta.lastPage = int(Number(root, lastPagePath_, 1));
    meta.total = Number(root, totalPath_, long long(items.size()));
    if (items.empty()) error = "the site returned no picture for this search";
    return !items.empty();
}

std::string OnlineSource::ExtensionFor(const std::string& url) const {
    const size_t stop = url.find_first_of("?#");
    const std::string body = url.substr(0, stop == std::string::npos ? std::string::npos : stop);
    const size_t slash = body.rfind('/');
    const size_t dot = body.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
        dot + 1 >= body.size() || body.size() - dot - 1 > 5)
        return defaultExt_;
    std::string ext = body.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (char c : ext)
        if (c < 'a' || c > 'z') return defaultExt_;
    return ext;
}

std::vector<OnlineSource> OnlineSource::LoadAll(const std::wstring& dir, std::vector<std::string>& skipped) {
    namespace fs = std::filesystem;
    std::vector<OnlineSource> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    std::vector<std::wstring> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && fs::path(e.path()).extension() == L".json") files.push_back(e.path().wstring());
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        const std::string name = ToUtf8(fs::path(file).filename().wstring());
        bool ok = false;
        const std::string text = ReadFileUtf8(file, ok);
        if (!ok) { skipped.push_back(name + ": cannot read"); continue; }
        std::string parseError;
        const auto parsed = Json::Parse(text, parseError);
        if (!parsed) { skipped.push_back(name + ": " + parseError); continue; }
        if (!parsed->isObject()) { skipped.push_back(name + ": not a JSON object"); continue; }
        const Json& j = *parsed;
        OnlineSource s;
        s.name_ = j.strOr("name");
        if (s.name_.empty()) { skipped.push_back(name + ": no 'name'"); continue; }
        s.searchUrl_ = j.strOr("search", j.strOr("url"));
        if (s.searchUrl_.empty()) { skipped.push_back(name + ": no 'search' url"); continue; }
        // A page with no api is described by the same file, with `record` naming the groups to read.
        s.recordRe_ = j.strOr("record");
        s.acceptsQuery_ = j.boolOr("ask", true);
        s.rollingPages_ = j.boolOr("rolling_pages", false);
        s.lastPageRe_ = j.strOr("last_page");
        s.slugFrom_ = j.strOr("slug_from");
        s.slugBasename_ = j.boolOr("slug_basename", false);
        s.slugCut_ = j.strOr("slug_cut");
        s.sizeFrom_ = j.strOr("size");
        if (!s.recordRe_.empty()) {
            // The page url is the listing, and the page number rides on it as a query the site itself
            // links to - `params` would put it in the query string of a search endpoint that is not here.
            std::regex probe;
            try { probe.assign(s.recordRe_); }
            catch (const std::regex_error& e) { skipped.push_back(name + ": bad 'record': " + e.what()); continue; }
            if (s.slugFrom_.empty() && s.recordRe_.find("{slug}") != std::string::npos)
                skipped.push_back(name + ": builds a url from {slug} but names no 'slug_from'");
        }
        s.label_ = j.strOr("label", s.name_);
        s.homepage_ = j.strOr("homepage");
        s.userAgent_ = j.strOr("user_agent", "Wallpaper/1.0 (desktop wallpaper client)");
        s.minIntervalMs_ = j.intOr("min_interval_ms", 2000);
        s.listPath_ = j.strOr("list", "data");
        s.pagePath_ = j.strOr("page", "meta.current_page");
        s.lastPagePath_ = j.strOr("last_page", "meta.last_page");
        s.totalPath_ = j.strOr("total", "meta.total");
        s.defaultExt_ = j.strOr("ext", "jpg");
        for (const auto& [k, v] : j.at("params").members())
            if (v.isString()) s.params_.emplace_back(k, v.asString());
        for (const auto& [k, v] : j.at("fields").members())
            if (v.isString()) s.fields_.emplace_back(k, v.asString());
        for (const auto& [k, v] : j.at("options").members()) {
            if (!v.isObject()) continue;
            OnlineOption o;
            o.key = k;
            for (const auto& it : v.at("values").items())
                if (it.isString()) o.values.push_back(it.asString());
            o.current = v.strOr("default", o.values.empty() ? std::string() : o.values.front());
            for (const auto& [lname, lab] : v.at("labels").members())
                if (lab.isString()) o.labels.emplace_back(lname, lab.asString());
            if (o.values.empty()) continue;
            if (std::find(o.values.begin(), o.values.end(), o.current) == o.values.end())
                o.current = o.values.front();
            s.options_.push_back(std::move(o));
        }
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace sw
