// VM2Backup — a small, safe XML parser and writer for SOAP (the vSphere Web Services API).
//   * element and attribute names are reduced to their LOCAL name (namespaces and prefixes are ignored; xmlns attributes are dropped;
//     only the XML Schema instance attributes keep a marker: "xsi:type"), so "soapenv:Body", "ns1:Body" and "Body" are the same thing;
//   * entities: the five predefined ones and numeric references; CDATA sections; comments and processing instructions are skipped;
//   * no DTD at all (a <!DOCTYPE is refused, so there are no external or user-defined entities and no entity expansion attack),
//     limits on the size, the nesting depth and the number of elements; damaged input gives an error, never a crash.
#pragma once
#include "base.h"

namespace vm2b {

struct XmlNode {
    Str name;                      // local name
    Str text;                      // the character data directly inside (all pieces joined, entities decoded)
    Vec<Str> attrNames, attrValues;// local names / decoded values
    Vec<XmlNode> kids;

    const XmlNode* child(const char* local) const;                    // the first child element of that name, or nullptr
    const XmlNode* find(const char* slashPath) const;                 // "a/b/c": first match at every level, or nullptr
    Str attr(const char* local, const char* def = "") const;
    bool hasAttr(const char* local) const;
    Str xsiType() const;                                              // xsi:type without its prefix ("" when absent); attr("xsi:type") is the raw one
    Str childText(const char* local, const char* def = "") const { const XmlNode* c = child(local); return c ? c->text : Str(def); }
    size_t countChildren(const char* local) const;
};

struct XmlLimits {
    size_t maxBytes, maxDepth, maxNodes;
    XmlLimits() : maxBytes((size_t)48 << 20), maxDepth(200), maxNodes(1500000) {}
};

// Parses one document; `root` becomes its root element. Error text is one line (with the byte offset).
bool XmlParse(const char* p, size_t n, XmlNode& root, Str& err, const XmlLimits& lim = XmlLimits());
inline bool XmlParse(const Str& s, XmlNode& root, Str& err) { return XmlParse(s.data(), s.size(), root, err); }

Str XmlEscape(const Str& s);       // & < > " ' ; control characters are dropped (they cannot appear in XML 1.0)

// Builds a document: open("a","x=\"1\"") / text / leaf / close. Attribute text is passed raw (the caller escapes with XmlEscape).
class XmlWriter {
public:
    XmlWriter& open(const char* name, const char* rawAttrs = nullptr);
    XmlWriter& close();                                        // closes the innermost open element
    XmlWriter& text(const Str& s);                             // escaped
    XmlWriter& leaf(const char* name, const Str& text, const char* rawAttrs = nullptr);
    XmlWriter& raw(const Str& xml) { m_out += xml; return *this; }
    Str str();                                                 // closes what is still open
private:
    Str m_out; Vec<Str> m_stack;
};

} // namespace vm2b
