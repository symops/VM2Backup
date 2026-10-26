#include "panelutil.h"

namespace vm2b {

Str PanelUrlHost(const Str& a)
{
    if (a.find(':') < 0) return a;                                   // IPv4 or a name
    Str r;
    for (size_t i = 0; i < a.size(); ++i) { if (a[i] == '%') r += "%25"; else r += a[i]; }          // a zone id is written %25<zone> in a URL
    return "[" + r + "]";
}

namespace {

bool IsLoopback4(const Str& a) { return a.starts_with("127."); }
bool IsWildcard(const Str& a) { return a == "0.0.0.0" || a == "::" || a == "[::]"; }
bool IsLinkLocal6(const Str& a) { Str l = a.lower(); return l.starts_with("fe8") || l.starts_with("fe9") || l.starts_with("fea") || l.starts_with("feb"); }

// ranks of a listening address: lower = better
int Rank(const Str& a)
{
    if (IsLoopback4(a)) return 0;
    if (a == "::1") return 1;
    if (IsWildcard(a)) return 2;                                      // reachable through loopback
    if (a.find(':') < 0) return 3;                                    // another IPv4
    if (!IsLinkLocal6(a)) return 4;
    return 5;
}

bool Pick(const Json& st, Str& host)
{
    int best = 99; Str bestAddr;
    const Json& es = st.get("entries");
    for (size_t i = 0; i < es.size(); ++i) {
        const Json& ad = es.at(i).get("addresses");
        for (size_t k = 0; k < ad.size(); ++k) {
            if (!ad.at(k).get("listening").boolean()) continue;
            Str a = ad.at(k).get("address").str(); if (a.empty()) continue;
            int r = Rank(a); if (r < best) { best = r; bestAddr = a; }
        }
    }
    if (best == 99) { Str a = st.get("address").str(); if (a.empty()) return false; bestAddr = a; best = Rank(a); }
    if (IsWildcard(bestAddr)) bestAddr = bestAddr.find(':') >= 0 ? "::1" : "127.0.0.1";
    host = bestAddr; return true;
}

} // namespace

PanelWebResult PanelWebUrl(const Json& st, Str& url)
{
    url = Str();
    if (!st.get("enabled").boolean()) return PANEL_WEB_OFF;
    if (!st.get("listening").boolean()) return PANEL_WEB_NOT_LISTENING;
    Str host; if (!Pick(st, host)) return PANEL_WEB_NOT_LISTENING;
    long long port = (long long)st.get("port").integer(8443); if (port < 1 || port > 65535) port = 8443;
    url = Str("https://") + PanelUrlHost(host) + Str::fmt(":%lld/", port);
    return PANEL_WEB_OK;
}

} // namespace vm2b
