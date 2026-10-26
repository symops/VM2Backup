// XML parser / writer, see xml.h.
#include "xml.h"

namespace vm2b {

const XmlNode* XmlNode::child(const char* local) const
{
    for (size_t i = 0; i < kids.size(); ++i) if (kids[i].name == local) return &kids[i];
    return nullptr;
}

const XmlNode* XmlNode::find(const char* path) const
{
    const XmlNode* cur = this;
    const char* p = path;
    while (cur && *p) {
        const char* e = strchr(p, '/'); size_t n = e ? (size_t)(e - p) : strlen(p);
        const XmlNode* next = nullptr;
        for (size_t i = 0; i < cur->kids.size(); ++i) if (cur->kids[i].name.size() == n && memcmp(cur->kids[i].name.data(), p, n) == 0) { next = &cur->kids[i]; break; }
        cur = next;
        if (!e) break;
        p = e + 1;
    }
    return cur;
}

Str XmlNode::attr(const char* local, const char* def) const
{
    for (size_t i = 0; i < attrNames.size(); ++i) if (attrNames[i] == local) return attrValues[i];
    return Str(def);
}

bool XmlNode::hasAttr(const char* local) const
{
    for (size_t i = 0; i < attrNames.size(); ++i) if (attrNames[i] == local) return true;
    return false;
}

Str XmlNode::xsiType() const
{
    Str t = attr("xsi:type");
    int c = t.rfind(':');
    return c >= 0 ? t.substr((size_t)c + 1) : t;
}

size_t XmlNode::countChildren(const char* local) const
{
    size_t n = 0;
    for (size_t i = 0; i < kids.size(); ++i) if (kids[i].name == local) ++n;
    return n;
}

namespace {

struct Parser {
    const char* p; size_t n, pos; const XmlLimits& lim; size_t nodes; Str err;
    Vec<Str> nsPrefix, nsUri;                  // the namespace declarations in scope (only needed to recognise xsi:type)
    Parser(const char* d, size_t len, const XmlLimits& l) : p(d), n(len), pos(0), lim(l), nodes(0) {}

    bool fail(const char* what) { if (err.empty()) err = Str::fmt("bad XML at byte %llu: %s", (unsigned long long)pos, what); return false; }
    // position of `s` in the data from `from` on, or -1 (no copy of the data)
    int Find(const char* s, size_t from) const {
        size_t k = strlen(s);
        for (size_t i = from; i + k <= n; ++i) {
            const void* q = memchr(p + i, s[0], n - i - k + 1);
            if (!q) return -1;
            i = (size_t)((const char*)q - p);
            if (memcmp(p + i, s, k) == 0) return (int)i;
        }
        return -1;
    }
    bool more() const { return pos < n; }
    bool looking(const char* s) const { size_t k = strlen(s); return pos + k <= n && memcmp(p + pos, s, k) == 0; }
    void skipWs() { while (pos < n && (p[pos] == ' ' || p[pos] == '\t' || p[pos] == '\r' || p[pos] == '\n')) ++pos; }
    static bool nameStart(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c >= 0x80; }
    static bool nameChar(unsigned char c) { return nameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }

    bool readName(Str& qname) {
        if (!more() || !nameStart((unsigned char)p[pos])) return fail("a name was expected");
        size_t s = pos;
        while (pos < n && nameChar((unsigned char)p[pos])) ++pos;
        qname = Str(p + s, pos - s);
        return true;
    }
    static Str Local(const Str& q) { int c = q.rfind(':'); return c >= 0 ? q.substr((size_t)c + 1) : q; }

    static void PutUtf8(Str& out, unsigned long cp) {
        char b[4];
        if (cp < 0x80) { b[0] = (char)cp; out.append(b, 1); }
        else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); out.append(b, 2); }
        else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); out.append(b, 3); }
        else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); out.append(b, 4); }
    }

    // at '&': decodes the reference into out
    bool entity(Str& out) {
        size_t e = pos + 1;
        while (e < n && e - pos <= 12 && p[e] != ';') ++e;
        if (e >= n || p[e] != ';') return fail("unterminated entity");
        Str ref(p + pos + 1, e - pos - 1);
        if (ref == "lt") out += '<';
        else if (ref == "gt") out += '>';
        else if (ref == "amp") out += '&';
        else if (ref == "quot") out += '"';
        else if (ref == "apos") out += '\'';
        else if (ref.size() >= 2 && ref[0] == '#') {
            unsigned long cp = 0; bool hex = ref[1] == 'x' || ref[1] == 'X';
            size_t i = hex ? 2 : 1;
            if (i >= ref.size()) return fail("bad character reference");
            for (; i < ref.size(); ++i) {
                char c = ref[i]; int d = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) return fail("bad character reference");
                cp = cp * (hex ? 16 : 10) + (unsigned long)d;
                if (cp > 0x10FFFF) return fail("character reference out of range");
            }
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("character reference not allowed");
            PutUtf8(out, cp);
        } else return fail("unknown entity (DTD entities are not supported)");
        pos = e + 1;
        return true;
    }

    bool attrValue(Str& out) {
        if (!more() || (p[pos] != '"' && p[pos] != '\'')) return fail("attribute value must be quoted");
        char q = p[pos++];
        while (more() && p[pos] != q) {
            if (p[pos] == '<') return fail("'<' in an attribute value");
            if (p[pos] == '&') { if (!entity(out)) return false; }
            else out += p[pos++];
        }
        if (!more()) return fail("unterminated attribute value");
        ++pos;
        return true;
    }

    bool skipMisc() {                         // comments, processing instructions, whitespace; refuses a DOCTYPE
        for (;;) {
            skipWs();
            if (looking("<!--")) { int e = Find("-->", pos + 4); if (e < 0) return fail("unterminated comment"); pos = (size_t)e + 3; }
            else if (looking("<?")) { int e = Find("?>", pos + 2); if (e < 0) return fail("unterminated processing instruction"); pos = (size_t)e + 2; }
            else if (looking("<!DOCTYPE") || looking("<!doctype")) return fail("DTDs are not allowed");
            else return true;
        }
    }

    // xmlns declarations go into the scope; attributes keep their local name, except the XML Schema instance ones, which are
    // stored as "xsi:<local>" (a MoRef has a plain type="VirtualMachine" and may also have xsi:type="ManagedObjectReference")
    void resolveAttrs(XmlNode& out, const Vec<Str>& names, const Vec<Str>& values) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i].starts_with("xmlns:")) { nsPrefix.push_back(names[i].substr(6)); nsUri.push_back(values[i]); }
            else if (names[i] == "xmlns") { nsPrefix.push_back(Str()); nsUri.push_back(values[i]); }
        }
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == "xmlns" || names[i].starts_with("xmlns:")) continue;
            int c = names[i].rfind(':');
            Str local = c >= 0 ? names[i].substr((size_t)c + 1) : names[i];
            bool xsi = false;
            if (c >= 0) {
                Str pre = names[i].substr(0, (size_t)c);
                bool found = false;
                for (size_t k = nsPrefix.size(); k-- > 0;) if (nsPrefix[k] == pre) { xsi = nsUri[k] == "http://www.w3.org/2001/XMLSchema-instance"; found = true; break; }
                if (!found && pre == "xsi") xsi = true;
            }
            out.attrNames.push_back(xsi ? "xsi:" + local : local); out.attrValues.push_back(values[i]);
        }
    }

    // pos at '<' of a start tag
    bool element(XmlNode& out, size_t depth) {
        if (depth > lim.maxDepth) return fail("nesting too deep");
        if (++nodes > lim.maxNodes) return fail("too many elements");
        ++pos;
        Str q; if (!readName(q)) return false;
        out.name = Local(q);
        size_t nsMark = nsPrefix.size();
        Vec<Str> rawNames, rawValues;
        for (;;) {
            skipWs();
            if (!more()) return fail("unterminated tag");
            if (p[pos] == '>') { ++pos; break; }
            if (p[pos] == '/') {
                if (pos + 1 < n && p[pos + 1] == '>') { pos += 2; resolveAttrs(out, rawNames, rawValues); nsPrefix.resize(nsMark); nsUri.resize(nsMark); return true; }
                return fail("bad tag end");
            }
            Str an; if (!readName(an)) return false;
            skipWs();
            if (!more() || p[pos] != '=') return fail("'=' expected after an attribute name");
            ++pos; skipWs();
            Str av; if (!attrValue(av)) return false;
            if (rawNames.size() >= 4096) return fail("too many attributes");
            rawNames.push_back(an); rawValues.push_back(av);
        }
        resolveAttrs(out, rawNames, rawValues);
        // content
        for (;;) {
            if (!more()) return fail("unexpected end of document inside an element");
            if (p[pos] == '<') {
                if (looking("</")) {
                    pos += 2; Str e; if (!readName(e)) return false;
                    if (Local(e) != out.name) return fail("mismatched end tag");
                    skipWs();
                    if (!more() || p[pos] != '>') return fail("'>' expected");
                    ++pos;
                    nsPrefix.resize(nsMark); nsUri.resize(nsMark);
                    return true;
                }
                if (looking("<!--")) { int e = Find("-->", pos + 4); if (e < 0) return fail("unterminated comment"); pos = (size_t)e + 3; continue; }
                if (looking("<![CDATA[")) {
                    size_t s = pos + 9; int e = Find("]]>", s);
                    if (e < 0) return fail("unterminated CDATA");
                    out.text.append(p + s, (size_t)e - s); pos = (size_t)e + 3; continue;
                }
                if (looking("<?")) { int e = Find("?>", pos + 2); if (e < 0) return fail("unterminated processing instruction"); pos = (size_t)e + 2; continue; }
                if (looking("<!")) return fail("DTD / markup declarations are not allowed");
                XmlNode k;
                if (!element(k, depth + 1)) return false;
                out.kids.push_back(static_cast<XmlNode&&>(k));
            } else if (p[pos] == '&') {
                if (!entity(out.text)) return false;
            } else {
                size_t s = pos;
                while (pos < n && p[pos] != '<' && p[pos] != '&') ++pos;
                out.text.append(p + s, pos - s);
            }
        }
    }
};

} // namespace

bool XmlParse(const char* data, size_t len, XmlNode& root, Str& err, const XmlLimits& lim)
{
    root = XmlNode();
    if (len > lim.maxBytes) { err = "XML document too large"; return false; }
    Parser ps(data, len, lim);
    if (len >= 3 && (u8)data[0] == 0xEF && (u8)data[1] == 0xBB && (u8)data[2] == 0xBF) ps.pos = 3;
    if (!ps.skipMisc()) { err = ps.err; return false; }
    if (!ps.more() || data[ps.pos] != '<') { err = "bad XML: no root element"; return false; }
    XmlNode r;
    if (!ps.element(r, 1)) { err = ps.err; return false; }
    if (!ps.skipMisc()) { err = ps.err; return false; }
    if (ps.more()) { err = "bad XML: data after the root element"; return false; }
    root = static_cast<XmlNode&&>(r);
    return true;
}

Str XmlEscape(const Str& s)
{
    Str o; o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&apos;"; break;
        default: if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') break; o += (char)c;
        }
    }
    return o;
}

XmlWriter& XmlWriter::open(const char* name, const char* rawAttrs)
{
    m_out += '<'; m_out += name;
    if (rawAttrs && *rawAttrs) { m_out += ' '; m_out += rawAttrs; }
    m_out += '>';
    m_stack.push_back(Str(name));
    return *this;
}

XmlWriter& XmlWriter::close()
{
    if (m_stack.empty()) return *this;
    m_out += "</"; m_out += m_stack.back(); m_out += '>';
    m_stack.pop_back();
    return *this;
}

XmlWriter& XmlWriter::text(const Str& s) { m_out += XmlEscape(s); return *this; }

XmlWriter& XmlWriter::leaf(const char* name, const Str& t, const char* rawAttrs)
{
    open(name, rawAttrs); text(t); return close();
}

Str XmlWriter::str()
{
    while (!m_stack.empty()) close();
    return m_out;
}

} // namespace vm2b
