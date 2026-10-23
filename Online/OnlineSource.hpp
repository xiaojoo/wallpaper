#pragma once
// Online/OnlineSource.hpp - one wallpaper site described as data instead of code.
//
// The shape is borrowed from a novel reader's book-source: the file names the endpoint, the query
// parameters, and which keys of the reply to read. What is deliberately NOT borrowed is the embedded
// script engine those readers ship - a JavaScript runtime in a desktop wallpaper app is a dependency
// and an attack surface for what is, here, one URL template and a handful of field paths.
//
// Nothing in this file touches the network or Qt: it loads a description and turns it into a request
// and into items, so the parse rules can be read without a running app.
#include "Engine/Core/Json.hpp"

#include <string>
#include <utility>
#include <vector>

namespace sw {

// One wallpaper the site offered. Every string is already resolved out of the reply.
struct OnlineItem {
    std::string key;      // the site's own id: the cache filename and the "already have it" check
    std::string title;
    std::string thumb;    // small enough to look at without downloading anything
    std::string image;    // the file 入库 fetches
    // A cheap full-size picture to preview `image` with, for a source whose `image` is not a picture.
    // A clip's original is the whole video and the hero is only ever looked at, so the site's own
    // frame of it stands in: motionbgs serves the 4K frame of a card for 405 KB against 19-29 MB for
    // the .mp4. Empty means "preview `image` itself", which is every picture source.
    std::string hero;
    std::string page;     // the site's page for it - what goes into the manifest as `origin`
    int width = 0;
    int height = 0;
    long long bytes = 0;
    std::string mime;
};

struct OnlineMeta {
    int page = 1;
    int lastPage = 1;
    long long total = 0;
    // False for a site whose pager links only its neighbour pages: it can answer "is there a next
    // one", never "how many there are". motionbgs is one (measured 2026-10-14: /4k/ runs to page 213,
    // and page 1 prints nothing but a Next arrow). The window draws a bare page number instead of a
    // numbered window when this is false, because the last cell of that window would be a lie.
    bool pagesKnown = true;
    // Records this page carried but this side refused to name a file after - see KeyIsSafe in the .cpp.
    int skippedKeys = 0;
    std::string skippedExample;     // the first key that was refused, so the log line is diagnosable
};

// One of the choices a site offers (a resolution floor, a sort order). `values` is in the order the
// page should show them; `current` is what the source file defaulted it to.
struct OnlineOption {
    std::string key;
    std::vector<std::string> values;
    std::string current;
    // What the site calls each value. A section named "graphics-cgi" reads as CGI on a card, and
    // these names come from the site, so they are data in the description rather than strings to
    // translate in the program.
    std::vector<std::pair<std::string, std::string>> labels;
    // Some orders only exist once the reader has typed something. wallhaven answers an empty box with
    // an EMPTY page while `meta.total` still claims 480208 matches on 20009 pages, for `relevance`,
    // `newest` and `top` alike (measured 2026-10-17) - a relevance order has nothing to be relevant
    // to. `needsQuery` names those values in the source file, and `withoutQuery` is what goes out
    // while the box is empty. The chip keeps showing his choice; the substitution is reported where
    // a page really does come back empty, not hidden behind a grid that looks merely small.
    std::vector<std::string> needsQuery;
    std::string withoutQuery;
    std::string labelFor(const std::string& value) const {
        for (const auto& [v, l] : labels)
            if (v == value) return l;
        return value;
    }
};

class OnlineSource {
public:
    // Every *.json in `dir`, sorted by name. A file that does not describe a source is reported in
    // `skipped` and the others still load - one broken description must not take the page away.
    static std::vector<OnlineSource> LoadAll(const std::wstring& dir, std::vector<std::string>& skipped);

    const std::string& name() const { return name_; }
    const std::string& label() const { return label_; }
    const std::string& homepage() const { return homepage_; }
    const std::string& userAgent() const { return userAgent_; }
    // The shortest gap this site is asked twice for. Their own reply header said 45 requests per
    // minute; this is the local half of obeying it.
    int minIntervalMs() const { return minIntervalMs_; }
    // False when the site has no text query a robot may use - 4kwallpapers puts its search under
    // /search/, which its own robots.txt disallows. The page hides the box instead of greying it,
    // because a greyed box promises a thing the site will never answer.
    bool acceptsQuery() const { return acceptsQuery_; }
    // True when the site's own pager only links the neighbouring page, so no total can ever be read
    // off a listing. Declared by the description file, not guessed: a page that happens to print one
    // link today is not the same thing as a site that publishes no total.
    bool rollingPages() const { return rollingPages_; }
    // Non-empty when this site is read out of a web page instead of a JSON reply.
    const std::string& record() const { return recordRe_; }
    // Non-empty when the full-size file is only written on the wallpaper's own page: one regex, one
    // capture group, run against `OnlineItem::page` before the download. See `OnlineFetcher::download`.
    const std::string& original() const { return originalRe_; }
    const std::vector<OnlineOption>& options() const { return options_; }
    bool setOption(const std::string& key, const std::string& value);
    std::string optionValue(const std::string& key) const;

    struct Request {
        std::string url;
        std::vector<std::pair<std::string, std::string>> query;   // encoded by the caller, not here
    };
    // A page of results for `userQuery`. Parameters whose expanded value is empty are left out, so an
    // empty search box sends no `q` at all instead of asking the site to match on nothing.
    Request BuildSearch(const std::string& userQuery, int page) const;

    // Reads one search reply into items. `error` says which part of the reply was missing, so a site
    // that changed its JSON fails with a name instead of an empty grid.
    bool ParseSearch(const Json& root, int askedPage, std::vector<OnlineItem>& items, OnlineMeta& meta,
                     std::string& error) const;
    // A site that answers with a web page instead of JSON. `record` is one regex with capture groups;
    // a field whose description starts with '$' is one of those groups, and anything else is a
    // template filled from the record's own fields, the options and the site's homepage. That is the
    // whole language on purpose: it buys "one site = one file" for a page with no api, and it stays
    // readable without a parser.
    bool ParseHtml(const std::string& body, int askedPage, std::vector<OnlineItem>& items,
                   OnlineMeta& meta, std::string& error) const;

    // jpg/png/... for the cached file: the URL's own suffix when it has one, else the default.
    std::string ExtensionFor(const std::string& url) const;

private:
    std::string Expand(const std::string& value, const std::string& userQuery, int page) const;
    // The same placeholders, plus the fields this very record captured.
    std::string ExpandItem(const std::string& value, const std::vector<std::pair<std::string, std::string>>& vars) const;
    std::string FieldOf(const char* name) const;

    std::string name_, label_, homepage_, userAgent_;
    std::string searchUrl_, listPath_, pagePath_, lastPagePath_, totalPath_, defaultExt_;
    int minIntervalMs_ = 2000;
    bool acceptsQuery_ = true;
    bool rollingPages_ = false;
    std::string recordRe_, lastPageRe_, slugFrom_, slugCut_, sizeFrom_, originalRe_;
    bool slugBasename_ = false;
    std::vector<std::pair<std::string, std::string>> params_;      // name -> value template
    std::vector<std::pair<std::string, std::string>> fields_;      // item field -> path in the reply
    std::vector<OnlineOption> options_;
};

} // namespace sw
