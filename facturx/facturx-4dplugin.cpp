/*
 * facturx -- 4D plugin: embed a Factur-X / ZUGFeRD invoice into a PDF/A-3 document.
 *
 * Backend: qpdf (https://github.com/qpdf/qpdf), Apache License 2.0.
 * Apache-2.0 permits static linking into closed-source commercial software
 * provided the licence text and NOTICE are distributed with the product.
 */

#include "facturx-4dplugin.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFEFStreamObjectHelper.hh>
#include <qpdf/QPDFFileSpecObjectHelper.hh>
#include <qpdf/QPDFEmbeddedFileDocumentHelper.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QUtil.hh>
#include <qpdf/Buffer.hh>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

/* ------------------------------------------------------------------ */
/* error codes                                                        */
/* ------------------------------------------------------------------ */

enum fx_code {
    FX_OK = 0,

    /* 1xx -- input validation */
    FX_BAD_ARITY = 100,
    FX_IN_PATH_EMPTY = 101,
    FX_IN_PDF_UNREADABLE = 102,
    FX_XMLINFO_NOT_OBJECT = 103,
    FX_XML_PATH_MISSING = 104,
    FX_XML_UNREADABLE = 105,
    FX_XML_EMPTY = 106,
    FX_XML_NOT_UTF8 = 107,
    FX_XML_NAME_INVALID = 108,
    FX_XML_RELATIONSHIP_INVALID = 109,
    FX_XML_DOCTYPE_INVALID = 110,
    FX_XML_VERSION_MISSING = 111,
    FX_XML_CONFORMANCE_MISSING = 112,
    FX_OUT_PATH_EMPTY = 113,
    FX_ATTACHMENTS_NOT_COLLECTION = 114,
    FX_ATTACHMENT_NOT_OBJECT = 115,
    FX_ATTACHMENT_UNREADABLE = 116,
    FX_ATTACHMENT_NAME_INVALID = 117,
    FX_ATTACHMENT_NAME_DUPLICATE = 118,
    FX_ATTACHMENT_TYPE_INVALID = 119,
    FX_ATTACHMENT_RELATIONSHIP_INVALID = 120,
    FX_OPTIONS_NOT_OBJECT = 121,
    FX_PROPERTY_WRONG_TYPE = 122,

    /* 2xx -- source PDF conformance */
    FX_PDF_PARSE_FAILED = 201,
    FX_PDF_ENCRYPTED = 202,
    FX_PDF_ALREADY_FACTURX = 203,
    FX_PDF_NO_XMP = 204,
    FX_PDF_VERSION_TOO_OLD = 205,
    FX_PDF_NAME_COLLISION = 206,
    FX_PDF_NOT_PDFA3 = 207,

    /* 3xx -- output */
    FX_OUT_EXISTS = 301,
    FX_OUT_DIR_UNWRITABLE = 302,
    FX_OUT_TEMP_FAILED = 303,
    FX_OUT_RENAME_FAILED = 304,
    FX_OUT_INPLACE_NO_OVERWRITE = 305,

    /* 4xx -- backend / internal */
    FX_BACKEND_ERROR = 401,
    FX_UNEXPECTED_EXCEPTION = 402,
    FX_UNKNOWN_FATAL = 403
};

namespace {

class FxError: public std::exception
{
  public:
    FxError(int code, std::string message) :
        code_(code),
        message_(std::move(message))
    {
    }
    int code() const { return code_; }
    std::string const& message() const { return message_; }
    char const* what() const noexcept override { return message_.c_str(); }

  private:
    int code_;
    std::string message_;
};

[[noreturn]] void
fail(int code, std::string const& message)
{
    throw FxError(code, message);
}

/* ------------------------------------------------------------------ */
/* 4D string helpers                                                  */
/* ------------------------------------------------------------------ */

std::vector<PA_Unichar>
utf8_to_utf16(std::string const& s)
{
    std::vector<PA_Unichar> out;
    if (!s.empty()) {
        std::vector<char> buf((s.size() + 1) * sizeof(PA_Unichar) * 2, 0);
        PA_long32 n = PA_ConvertCharsetToCharset(
            const_cast<char*>(s.data()),
            static_cast<PA_long32>(s.size()),
            eVTC_UTF_8,
            buf.data(),
            static_cast<PA_long32>(buf.size()),
            eVTC_UTF_16);
        size_t count = (n > 0 ? static_cast<size_t>(n) : 0) / sizeof(PA_Unichar);
        PA_Unichar const* p = reinterpret_cast<PA_Unichar const*>(buf.data());
        out.assign(p, p + count);
    }
    out.push_back(0);
    return out;
}

std::string
utf16_to_utf8(PA_Unistring const& u)
{
    if (u.fString == nullptr || u.fLength == 0) {
        return std::string();
    }
    size_t in_bytes = static_cast<size_t>(u.fLength) * sizeof(PA_Unichar);
    std::vector<char> buf(in_bytes * 3 + 8, 0);
    PA_long32 n = PA_ConvertCharsetToCharset(
        reinterpret_cast<char*>(u.fString),
        static_cast<PA_long32>(in_bytes),
        eVTC_UTF_16,
        buf.data(),
        static_cast<PA_long32>(buf.size()),
        eVTC_UTF_8);
    if (n <= 0 || static_cast<size_t>(n) > buf.size()) {
        return std::string(buf.data());
    }
    return std::string(buf.data(), static_cast<size_t>(n));
}

/* RAII wrapper around a PA_Unistring we own. */
class UStr
{
  public:
    explicit UStr(std::string const& s)
    {
        std::vector<PA_Unichar> w = utf8_to_utf16(s);
        u_ = PA_CreateUnistring(w.data());
    }
    ~UStr() { PA_DisposeUnistring(&u_); }
    UStr(UStr const&) = delete;
    UStr& operator=(UStr const&) = delete;
    PA_Unistring* get() { return &u_; }

  private:
    PA_Unistring u_;
};

/* RAII wrapper around a PA_Variable we must clear. */
class PAVar
{
  public:
    PAVar() { std::memset(&v_, 0, sizeof(v_)); }
    explicit PAVar(PA_Variable v) : v_(v) {}
    ~PAVar() { PA_ClearVariable(&v_); }
    PAVar(PAVar const&) = delete;
    PAVar& operator=(PAVar const&) = delete;
    PA_Variable& get() { return v_; }
    PA_VariableKind kind() { return PA_GetVariableKind(v_); }

  private:
    PA_Variable v_;
};

void
object_set_text(PA_ObjectRef obj, char const* key, std::string const& value)
{
    std::vector<PA_Unichar> w = utf8_to_utf16(value);
    PA_Unistring u = PA_CreateUnistring(w.data());
    PA_Variable v;
    std::memset(&v, 0, sizeof(v));
    PA_SetStringVariable(&v, &u);
    UStr k(key);
    PA_SetObjectProperty(obj, k.get(), v);
    PA_ClearVariable(&v);
}

void
object_set_long(PA_ObjectRef obj, char const* key, PA_long32 value)
{
    PA_Variable v;
    std::memset(&v, 0, sizeof(v));
    PA_SetLongintVariable(&v, value);
    UStr k(key);
    PA_SetObjectProperty(obj, k.get(), v);
    PA_ClearVariable(&v);
}

void
object_set_bool(PA_ObjectRef obj, char const* key, bool value)
{
    PA_Variable v;
    std::memset(&v, 0, sizeof(v));
    PA_SetBooleanVariable(&v, value ? 1 : 0);
    UStr k(key);
    PA_SetObjectProperty(obj, k.get(), v);
    PA_ClearVariable(&v);
}

void
object_set_collection(PA_ObjectRef obj, char const* key, PA_CollectionRef c)
{
    PA_Variable v;
    std::memset(&v, 0, sizeof(v));
    PA_SetCollectionVariable(&v, c);
    UStr k(key);
    PA_SetObjectProperty(obj, k.get(), v);
    PA_ClearVariable(&v);
}

/*
 * Read a property as a variable.
 *
 * PA_HasObjectProperty and PA_GetObjectPropertyType route through
 * PA_ExecuteCommandByID with an uninitialised PA_Variable array, which
 * segfaults inside 4D's parameter marshalling, so neither can be used.
 * PA_GetObjectProperty talks to the runtime through an EngineBlock and is
 * safe, and it reports an absent property as eVK_Undefined.
 */
PA_Variable
object_property(PA_ObjectRef obj, char const* key)
{
    UStr k(key);
    PA_Variable v = PA_GetObjectProperty(obj, k.get());
    if (PA_GetLastError() != eER_NoErr) {
        std::memset(&v, 0, sizeof(v));
    }
    return v;
}

/*
 * Read a Text property. Returns false when the property is absent or null.
 * Throws FX_PROPERTY_WRONG_TYPE when present with a non-text type.
 */
bool
object_get_text(PA_ObjectRef obj, char const* key, std::string& out, std::string const& ctx)
{
    PAVar v(object_property(obj, key));
    PA_VariableKind kind = v.kind();
    if (kind == eVK_Null || kind == eVK_Undefined) {
        return false;
    }
    if (kind != eVK_Unistring && kind != eVK_Text) {
        fail(
            FX_PROPERTY_WRONG_TYPE,
            ctx + "." + key + " must be Text (received 4D type " + std::to_string(kind) + ")");
    }
    PA_Unistring u = PA_GetStringVariable(v.get());
    out = utf16_to_utf8(u);
    return true;
}

bool
object_get_bool(PA_ObjectRef obj, char const* key, bool def, std::string const& ctx)
{
    PAVar v(object_property(obj, key));
    PA_VariableKind kind = v.kind();
    if (kind == eVK_Null || kind == eVK_Undefined) {
        return def;
    }
    if (kind != eVK_Boolean) {
        fail(
            FX_PROPERTY_WRONG_TYPE,
            ctx + "." + key + " must be Boolean (received 4D type " + std::to_string(kind) + ")");
    }
    return PA_GetBooleanVariable(v.get()) != 0;
}

/* ------------------------------------------------------------------ */
/* generic string helpers                                             */
/* ------------------------------------------------------------------ */

std::string
trim(std::string const& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return std::string();
    }
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string
lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool
ends_with(std::string const& s, std::string const& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string
xml_escape(std::string const& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c: s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

/* Strict UTF-8 well-formedness check (also rejects surrogates and over-longs). */
bool
is_valid_utf8(std::string const& s)
{
    size_t i = 0;
    size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        unsigned long cp = 0;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= n) {
            return false;
        }
        for (size_t j = 1; j <= extra; ++j) {
            unsigned char cc = static_cast<unsigned char>(s[i + j]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (extra == 1 && cp < 0x80) {
            return false;
        }
        if (extra == 2 && cp < 0x800) {
            return false;
        }
        if (extra == 3 && cp < 0x10000) {
            return false;
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string
read_whole_file(std::string const& path, int missing_code, std::string const& what)
{
    if (!QUtil::file_can_be_opened(path.c_str())) {
        fail(missing_code, what + " cannot be opened: " + path);
    }
    FILE* f = QUtil::safe_fopen(path.c_str(), "rb");
    if (f == nullptr) {
        fail(missing_code, what + " cannot be opened: " + path);
    }
    std::string data;
    char buf[65536];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        data.append(buf, got);
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) {
        fail(missing_code, what + " could not be read: " + path);
    }
    return data;
}

std::string
directory_of(std::string const& path)
{
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return std::string(".");
    }
    if (slash == 0) {
        return std::string("/");
    }
    return path.substr(0, slash);
}

/* Normalize for path equality comparison only -- not a canonicalisation. */
std::string
path_key(std::string const& p)
{
    std::string s = p;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
#if defined(_WIN32)
    s = lower(s);
#endif
    return s;
}

bool
has_path_separator(std::string const& s)
{
    return s.find('/') != std::string::npos || s.find('\\') != std::string::npos ||
        s.find(':') != std::string::npos;
}

/*
 * MIME types are written into a PDF name object. Restrict to the RFC 2045
 * token character set so nothing can break PDF name/string syntax.
 */
bool
is_valid_mime(std::string const& s)
{
    size_t slash = s.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size()) {
        return false;
    }
    if (s.find('/', slash + 1) != std::string::npos) {
        return false;
    }
    static char const* extra = "!#$%&'*+-.^_`|~";
    for (char c: s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '/') {
            continue;
        }
        if (std::isalnum(u)) {
            continue;
        }
        if (std::strchr(extra, c) != nullptr) {
            continue;
        }
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* XMP generation                                                     */
/* ------------------------------------------------------------------ */

char const* FX_NS_FACTURX = "urn:factur-x:pdfa:CrossIndustryDocument:invoice:1p0#";
char const* FX_NS_ZUGFERD = "urn:zugferd:pdfa:CrossIndustryDocument:invoice:2p0#";

struct XmlInfo
{
    std::string path;
    std::string name = "factur-x.xml";
    std::string relationship = "Alternative";
    std::string description = "Factur-X Invoice";
    std::string documentType = "INVOICE";
    std::string version;
    std::string conformanceLevel;
    std::string xmpPrefix;
    std::string xmpNamespace;
    std::string data;
};

struct Attachment
{
    std::string path;
    std::string name;
    std::string type;
    std::string relationship = "Unspecified";
    std::string description;
    std::string data;
};

struct Options
{
    bool overwrite = false;
    bool strict = false;
    bool forcePdfAPart3 = false;
};

std::string
build_fx_description(XmlInfo const& xi)
{
    std::ostringstream o;
    o << "  <rdf:Description rdf:about=\"\" xmlns:" << xi.xmpPrefix << "=\""
      << xml_escape(xi.xmpNamespace) << "\">\n"
      << "   <" << xi.xmpPrefix << ":DocumentType>" << xml_escape(xi.documentType) << "</"
      << xi.xmpPrefix << ":DocumentType>\n"
      << "   <" << xi.xmpPrefix << ":DocumentFileName>" << xml_escape(xi.name) << "</"
      << xi.xmpPrefix << ":DocumentFileName>\n"
      << "   <" << xi.xmpPrefix << ":Version>" << xml_escape(xi.version) << "</" << xi.xmpPrefix
      << ":Version>\n"
      << "   <" << xi.xmpPrefix << ":ConformanceLevel>" << xml_escape(xi.conformanceLevel) << "</"
      << xi.xmpPrefix << ":ConformanceLevel>\n"
      << "  </rdf:Description>\n";
    return o.str();
}

std::string
build_extension_li(XmlInfo const& xi)
{
    struct Prop
    {
        char const* name;
        char const* desc;
    };
    static Prop const props[] = {
        {"DocumentFileName", "name of the embedded XML invoice file"},
        {"DocumentType", "INVOICE or CREDITNOTE"},
        {"Version", "The actual version of the standard applying to the embedded XML document"},
        {"ConformanceLevel", "The conformance level of the embedded XML document"}};

    std::ostringstream o;
    o << "     <rdf:li rdf:parseType=\"Resource\""
      << " xmlns:pdfaSchema=\"http://www.aiim.org/pdfa/ns/schema#\""
      << " xmlns:pdfaProperty=\"http://www.aiim.org/pdfa/ns/property#\">\n"
      << "      <pdfaSchema:schema>Factur-X PDFA Extension Schema</pdfaSchema:schema>\n"
      << "      <pdfaSchema:namespaceURI>" << xml_escape(xi.xmpNamespace)
      << "</pdfaSchema:namespaceURI>\n"
      << "      <pdfaSchema:prefix>" << xi.xmpPrefix << "</pdfaSchema:prefix>\n"
      << "      <pdfaSchema:property>\n"
      << "       <rdf:Seq>\n";
    for (Prop const& p: props) {
        o << "        <rdf:li rdf:parseType=\"Resource\">\n"
          << "         <pdfaProperty:name>" << p.name << "</pdfaProperty:name>\n"
          << "         <pdfaProperty:valueType>Text</pdfaProperty:valueType>\n"
          << "         <pdfaProperty:category>external</pdfaProperty:category>\n"
          << "         <pdfaProperty:description>" << xml_escape(p.desc)
          << "</pdfaProperty:description>\n"
          << "        </rdf:li>\n";
    }
    o << "       </rdf:Seq>\n"
      << "      </pdfaSchema:property>\n"
      << "     </rdf:li>\n";
    return o.str();
}

std::string
build_extension_description(XmlInfo const& xi)
{
    std::ostringstream o;
    o << "  <rdf:Description rdf:about=\"\""
      << " xmlns:pdfaExtension=\"http://www.aiim.org/pdfa/ns/extension/\">\n"
      << "   <pdfaExtension:schemas>\n"
      << "    <rdf:Bag>\n"
      << build_extension_li(xi) << "    </rdf:Bag>\n"
      << "   </pdfaExtension:schemas>\n"
      << "  </rdf:Description>\n";
    return o.str();
}

std::string
build_new_xmp(XmlInfo const& xi)
{
    std::ostringstream o;
    o << "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
      << "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
      << " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
      << "  <rdf:Description rdf:about=\"\" xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\">\n"
      << "   <pdfaid:part>3</pdfaid:part>\n"
      << "   <pdfaid:conformance>B</pdfaid:conformance>\n"
      << "  </rdf:Description>\n"
      << build_extension_description(xi) << build_fx_description(xi) << " </rdf:RDF>\n"
      << "</x:xmpmeta>\n"
      << "<?xpacket end=\"w\"?>\n";
    return o.str();
}

/* Read pdfaid:part, in either element or attribute form. Returns "" if absent. */
std::string
read_pdfaid_part(std::string const& xmp)
{
    size_t p = xmp.find("<pdfaid:part>");
    if (p != std::string::npos) {
        size_t b = p + std::strlen("<pdfaid:part>");
        size_t e = xmp.find("</pdfaid:part>", b);
        if (e != std::string::npos) {
            return trim(xmp.substr(b, e - b));
        }
    }
    p = xmp.find("pdfaid:part=\"");
    if (p != std::string::npos) {
        size_t b = p + std::strlen("pdfaid:part=\"");
        size_t e = xmp.find('"', b);
        if (e != std::string::npos) {
            return trim(xmp.substr(b, e - b));
        }
    }
    return std::string();
}

std::string
force_pdfaid_part3(std::string xmp)
{
    size_t p = xmp.find("<pdfaid:part>");
    if (p != std::string::npos) {
        size_t b = p + std::strlen("<pdfaid:part>");
        size_t e = xmp.find("</pdfaid:part>", b);
        if (e != std::string::npos) {
            xmp.replace(b, e - b, "3");
            return xmp;
        }
    }
    p = xmp.find("pdfaid:part=\"");
    if (p != std::string::npos) {
        size_t b = p + std::strlen("pdfaid:part=\"");
        size_t e = xmp.find('"', b);
        if (e != std::string::npos) {
            xmp.replace(b, e - b, "3");
        }
    }
    return xmp;
}

/*
 * Extend an existing XMP packet rather than replacing it: existing pdfaid /
 * dc / xmp blocks are preserved untouched. The Factur-X description is
 * appended as an additional rdf:Description, and the PDF/A extension schema
 * entry is merged into an existing pdfaExtension:schemas bag when present.
 */
std::string
extend_xmp(std::string const& xmp, XmlInfo const& xi, std::vector<std::string>& warnings)
{
    std::string out = xmp;

    /* 1. PDF/A extension schema. */
    size_t schemas = out.find("<pdfaExtension:schemas");
    if (schemas != std::string::npos) {
        size_t bag = out.find("<rdf:Bag>", schemas);
        if (bag != std::string::npos) {
            out.insert(bag + std::strlen("<rdf:Bag>"), "\n" + build_extension_li(xi));
        } else {
            warnings.push_back(
                "source XMP has a pdfaExtension:schemas element with no rdf:Bag; the Factur-X "
                "extension schema description could not be merged");
        }
    } else {
        size_t rdf_end = out.rfind("</rdf:RDF>");
        if (rdf_end == std::string::npos) {
            return std::string();
        }
        out.insert(rdf_end, build_extension_description(xi));
    }

    /* 2. Factur-X description. */
    size_t rdf_end = out.rfind("</rdf:RDF>");
    if (rdf_end == std::string::npos) {
        return std::string();
    }
    out.insert(rdf_end, build_fx_description(xi));
    return out;
}

/* ------------------------------------------------------------------ */
/* PDF work                                                           */
/* ------------------------------------------------------------------ */

/* Names that unambiguously identify an already-embedded CTC invoice. */
bool
is_reserved_invoice_name(std::string const& name)
{
    std::string n = lower(name);
    return n == "factur-x.xml" || n == "zugferd-invoice.xml" || n == "xrechnung.xml" ||
        n == "factur-x.xml " || n == "zugferd-invoice.xml ";
}

/* Collect the file names already registered in the name tree and in /AF. */
std::set<std::string>
collect_existing_names(QPDF& pdf)
{
    std::set<std::string> names;

    QPDFEmbeddedFileDocumentHelper efdh(pdf);
    for (auto const& item: efdh.getEmbeddedFiles()) {
        names.insert(lower(item.first));
        if (item.second) {
            std::string fn = item.second->getFilename();
            if (!fn.empty()) {
                names.insert(lower(fn));
            }
        }
    }

    QPDFObjectHandle af = pdf.getRoot().getKey("/AF");
    if (af.isArray()) {
        int n = af.getArrayNItems();
        for (int i = 0; i < n; ++i) {
            QPDFObjectHandle item = af.getArrayItem(i);
            if (!item.isDictionary()) {
                continue;
            }
            QPDFFileSpecObjectHelper fs(item);
            std::string fn = fs.getFilename();
            if (!fn.empty()) {
                names.insert(lower(fn));
            }
        }
    }
    return names;
}

void
attach(
    QPDF& pdf,
    QPDFEmbeddedFileDocumentHelper& efdh,
    std::string const& name,
    std::string const& data,
    std::string const& mime,
    std::string const& relationship,
    std::string const& description,
    std::string const& now)
{
    QPDFEFStreamObjectHelper ef = QPDFEFStreamObjectHelper::createEFStream(pdf, data);
    ef.setSubtype(mime);
    ef.setCreationDate(now);
    ef.setModDate(now);

    QPDFFileSpecObjectHelper fs = QPDFFileSpecObjectHelper::createFileSpec(pdf, name, ef);
    if (!description.empty()) {
        fs.setDescription(description);
    }
    fs.getObjectHandle().replaceKey(
        "/AFRelationship", QPDFObjectHandle::newName("/" + relationship));

    /* Requirement: register in BOTH the name tree and /AF, same indirect object. */
    efdh.replaceEmbeddedFile(name, fs);

    QPDFObjectHandle root = pdf.getRoot();
    QPDFObjectHandle af = root.getKey("/AF");
    if (!af.isArray()) {
        af = pdf.makeIndirectObject(QPDFObjectHandle::newArray());
        root.replaceKey("/AF", af);
    }
    af.appendItem(fs.getObjectHandle());
}

/* ------------------------------------------------------------------ */
/* parameter parsing                                                  */
/* ------------------------------------------------------------------ */

XmlInfo
parse_xml_info(PA_ObjectRef obj, std::vector<std::string>& warnings)
{
    if (obj == nullptr) {
        fail(FX_XMLINFO_NOT_OBJECT, "xmlInfo must be a non-null Object");
    }
    XmlInfo xi;

    if (!object_get_text(obj, "path", xi.path, "xmlInfo") || trim(xi.path).empty()) {
        fail(FX_XML_PATH_MISSING, "xmlInfo.path is required and must be a non-empty Text value");
    }
    xi.path = trim(xi.path);

    std::string tmp;
    if (object_get_text(obj, "name", tmp, "xmlInfo") && !trim(tmp).empty()) {
        xi.name = trim(tmp);
    }
    if (has_path_separator(xi.name)) {
        fail(FX_XML_NAME_INVALID, "xmlInfo.name must not contain a path separator: " + xi.name);
    }
    if (!ends_with(lower(xi.name), ".xml")) {
        fail(FX_XML_NAME_INVALID, "xmlInfo.name must end in .xml: " + xi.name);
    }
    {
        std::string n = lower(xi.name);
        if (n != "factur-x.xml" && n != "zugferd-invoice.xml") {
            warnings.push_back(
                "xmlInfo.name \"" + xi.name +
                "\" is not one of the conventional Factur-X/ZUGFeRD reserved names "
                "(factur-x.xml, zugferd-invoice.xml); some readers will not detect the invoice");
        }
    }

    if (object_get_text(obj, "relationship", tmp, "xmlInfo") && !trim(tmp).empty()) {
        xi.relationship = trim(tmp);
    }
    if (xi.relationship != "Alternative" && xi.relationship != "Source") {
        fail(
            FX_XML_RELATIONSHIP_INVALID,
            "xmlInfo.relationship must be exactly \"Alternative\" or \"Source\", received \"" +
                xi.relationship + "\"");
    }

    if (object_get_text(obj, "description", tmp, "xmlInfo") && !trim(tmp).empty()) {
        xi.description = trim(tmp);
    }

    if (object_get_text(obj, "documentType", tmp, "xmlInfo") && !trim(tmp).empty()) {
        xi.documentType = trim(tmp);
    }
    if (xi.documentType != "INVOICE" && xi.documentType != "CREDITNOTE") {
        fail(
            FX_XML_DOCTYPE_INVALID,
            "xmlInfo.documentType must be \"INVOICE\" or \"CREDITNOTE\", received \"" +
                xi.documentType + "\"");
    }

    if (!object_get_text(obj, "version", xi.version, "xmlInfo") || trim(xi.version).empty()) {
        fail(
            FX_XML_VERSION_MISSING,
            "xmlInfo.version is required (e.g. \"1.0\"); it drives the XMP profile block");
    }
    xi.version = trim(xi.version);

    if (!object_get_text(obj, "conformanceLevel", xi.conformanceLevel, "xmlInfo") ||
        trim(xi.conformanceLevel).empty()) {
        fail(
            FX_XML_CONFORMANCE_MISSING,
            "xmlInfo.conformanceLevel is required (e.g. \"BASIC\", \"EN 16931\", \"EXTENDED\")");
    }
    xi.conformanceLevel = trim(xi.conformanceLevel);

    /* XMP namespace defaults follow the reserved file name. */
    if (lower(xi.name) == "zugferd-invoice.xml") {
        xi.xmpPrefix = "zf";
        xi.xmpNamespace = FX_NS_ZUGFERD;
    } else {
        xi.xmpPrefix = "fx";
        xi.xmpNamespace = FX_NS_FACTURX;
    }
    if (object_get_text(obj, "xmpPrefix", tmp, "xmlInfo") && !trim(tmp).empty()) {
        xi.xmpPrefix = trim(tmp);
    }
    if (object_get_text(obj, "xmpNamespace", tmp, "xmlInfo") && !trim(tmp).empty()) {
        xi.xmpNamespace = trim(tmp);
    }

    xi.data = read_whole_file(xi.path, FX_XML_UNREADABLE, "xmlInfo.path");
    if (xi.data.empty()) {
        fail(FX_XML_EMPTY, "xmlInfo.path refers to an empty file: " + xi.path);
    }
    if (xi.data.size() >= 3 && static_cast<unsigned char>(xi.data[0]) == 0xEF &&
        static_cast<unsigned char>(xi.data[1]) == 0xBB &&
        static_cast<unsigned char>(xi.data[2]) == 0xBF) {
        /* Decision: BOM is stripped and reported, not rejected -- many ERP
           exporters emit one and readers tolerate it, but Factur-X samples
           do not use one. */
        xi.data.erase(0, 3);
        warnings.push_back("xmlInfo.path started with a UTF-8 BOM; the BOM was removed");
    }
    if (!is_valid_utf8(xi.data)) {
        fail(FX_XML_NOT_UTF8, "xmlInfo.path is not valid UTF-8: " + xi.path);
    }
    return xi;
}

std::vector<Attachment>
parse_attachments(PA_CollectionRef col, XmlInfo const& xi)
{
    std::vector<Attachment> result;
    if (col == nullptr) {
        return result;
    }
    PA_long32 count = PA_GetCollectionLength(col);
    std::set<std::string> seen;
    seen.insert(lower(xi.name));

    for (PA_long32 i = 0; i < count; ++i) {
        std::string ctx = "attachments[" + std::to_string(i) + "]";
        PAVar v(PA_GetCollectionElement(col, i));
        if (v.kind() != eVK_Object) {
            fail(FX_ATTACHMENT_NOT_OBJECT, ctx + " must be an Object");
        }
        PA_ObjectRef o = PA_GetObjectVariable(v.get());
        if (o == nullptr) {
            fail(FX_ATTACHMENT_NOT_OBJECT, ctx + " must be a non-null Object");
        }

        Attachment a;
        if (!object_get_text(o, "path", a.path, ctx) || trim(a.path).empty()) {
            fail(FX_ATTACHMENT_UNREADABLE, ctx + ".path is required and must be a Text value");
        }
        a.path = trim(a.path);

        if (!object_get_text(o, "name", a.name, ctx) || trim(a.name).empty()) {
            fail(FX_ATTACHMENT_NAME_INVALID, ctx + ".name is required and must be a Text value");
        }
        a.name = trim(a.name);
        if (has_path_separator(a.name)) {
            fail(
                FX_ATTACHMENT_NAME_INVALID,
                ctx + ".name must not contain a path separator: " + a.name);
        }
        if (!seen.insert(lower(a.name)).second) {
            fail(
                FX_ATTACHMENT_NAME_DUPLICATE,
                ctx + ".name \"" + a.name +
                    "\" duplicates another attachment name or xmlInfo.name "
                    "(comparison is case-insensitive)");
        }

        if (!object_get_text(o, "type", a.type, ctx) || trim(a.type).empty()) {
            fail(
                FX_ATTACHMENT_TYPE_INVALID,
                ctx + ".type is required and must be a MIME type Text value");
        }
        a.type = trim(a.type);
        if (!is_valid_mime(a.type)) {
            fail(FX_ATTACHMENT_TYPE_INVALID, ctx + ".type is not a usable MIME type: " + a.type);
        }

        std::string tmp;
        if (object_get_text(o, "relationship", tmp, ctx) && !trim(tmp).empty()) {
            a.relationship = trim(tmp);
        }
        if (a.relationship != "Supplement" && a.relationship != "Data" &&
            a.relationship != "Unspecified") {
            fail(
                FX_ATTACHMENT_RELATIONSHIP_INVALID,
                ctx + ".relationship must be \"Supplement\", \"Data\" or \"Unspecified\" "
                      "(\"Alternative\" and \"Source\" are reserved for the invoice XML), "
                      "received \"" +
                    a.relationship + "\"");
        }

        object_get_text(o, "description", a.description, ctx);
        a.description = trim(a.description);

        a.data = read_whole_file(a.path, FX_ATTACHMENT_UNREADABLE, ctx + ".path");
        result.push_back(std::move(a));
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* command body                                                       */
/* ------------------------------------------------------------------ */

struct Result
{
    int code = FX_OK;
    std::string message;
    std::vector<std::string> warnings;
};

void
run(PA_PluginParameters params, Result& result)
{
    std::vector<std::string>& warnings = result.warnings;

    /* --- 1. parameters ------------------------------------------------ */
    std::string in_path;
    std::string out_path;
    {
        PA_Unistring* u = PA_GetStringParameter(params, 1);
        if (u != nullptr) {
            in_path = trim(utf16_to_utf8(*u));
        }
        u = PA_GetStringParameter(params, 3);
        if (u != nullptr) {
            out_path = trim(utf16_to_utf8(*u));
        }
    }
    if (in_path.empty()) {
        fail(FX_IN_PATH_EMPTY, "pdfPathIn is required and must be a non-empty POSIX path");
    }
    if (out_path.empty()) {
        fail(FX_OUT_PATH_EMPTY, "pdfPathOut is required and must be a non-empty POSIX path");
    }

    PA_ObjectRef xml_obj = PA_GetObjectParameter(params, 2);
    XmlInfo xi = parse_xml_info(xml_obj, warnings);

    PA_CollectionRef att_col = PA_GetCollectionParameter(params, 4);
    std::vector<Attachment> attachments = parse_attachments(att_col, xi);

    Options opt;
    PA_ObjectRef opt_obj = PA_GetObjectParameter(params, 5);
    if (opt_obj != nullptr) {
        opt.overwrite = object_get_bool(opt_obj, "overwrite", false, "options");
        opt.strict = object_get_bool(opt_obj, "strict", false, "options");
        opt.forcePdfAPart3 = object_get_bool(opt_obj, "forcePdfAPart3", false, "options");
    }

    if (!QUtil::file_can_be_opened(in_path.c_str())) {
        fail(FX_IN_PDF_UNREADABLE, "pdfPathIn cannot be opened: " + in_path);
    }

    /* --- 2. output path policy --------------------------------------- */
    bool in_place = path_key(in_path) == path_key(out_path);
    bool out_exists = QUtil::file_can_be_opened(out_path.c_str());

    if (in_place && !opt.overwrite) {
        fail(
            FX_OUT_INPLACE_NO_OVERWRITE,
            "pdfPathOut is the same file as pdfPathIn; in-place editing is supported but "
            "requires options.overwrite:=True");
    }
    if (out_exists && !opt.overwrite) {
        fail(
            FX_OUT_EXISTS,
            "pdfPathOut already exists and options.overwrite is not True: " + out_path);
    }

    /* --- 3. load and pre-flight the source PDF ------------------------ */
    QPDF pdf;
    pdf.setSuppressWarnings(true);
    try {
        pdf.processFile(in_path.c_str());
    } catch (QPDFExc const& e) {
        fail(FX_PDF_PARSE_FAILED, std::string("source PDF could not be parsed: ") + e.what());
    }
    if (pdf.isEncrypted()) {
        fail(
            FX_PDF_ENCRYPTED,
            "source PDF is encrypted; PDF/A-3 and Factur-X documents must not be encrypted");
    }

    std::string pdf_version = pdf.getPDFVersion();
    bool version_too_old = false;
    {
        double v = 0.0;
        try {
            v = std::stod(pdf_version);
        } catch (...) {
            v = 0.0;
        }
        version_too_old = v > 0.0 && v < 1.7;
    }
    if (version_too_old) {
        if (opt.strict) {
            fail(
                FX_PDF_VERSION_TOO_OLD,
                "source PDF declares version " + pdf_version +
                    "; /AF requires PDF 1.7 or later (options.strict)");
        }
        warnings.push_back(
            "source PDF declares version " + pdf_version +
            "; the output was upgraded to 1.7 because the /AF array requires it");
    }

    /* Reject re-processing rather than producing two conflicting invoices. */
    std::set<std::string> existing = collect_existing_names(pdf);
    for (std::string const& n: existing) {
        if (is_reserved_invoice_name(n) || n == lower(xi.name)) {
            fail(
                FX_PDF_ALREADY_FACTURX,
                "source PDF already embeds \"" + n +
                    "\"; re-running would produce a document with two conflicting invoices");
        }
    }
    for (Attachment const& a: attachments) {
        if (existing.count(lower(a.name)) != 0) {
            fail(
                FX_PDF_NAME_COLLISION,
                "attachment name \"" + a.name +
                    "\" collides with a file already embedded in the source PDF");
        }
    }

    /* --- 4. XMP ------------------------------------------------------- */
    QPDFObjectHandle root = pdf.getRoot();
    QPDFObjectHandle meta = root.getKey("/Metadata");
    std::string xmp;
    bool had_xmp = false;
    if (meta.isStream()) {
        try {
            std::shared_ptr<Buffer> b = meta.getStreamData(qpdf_dl_all);
            xmp.assign(reinterpret_cast<char const*>(b->getBuffer()), b->getSize());
            had_xmp = true;
        } catch (std::exception const&) {
            had_xmp = false;
        }
    }

    if (!had_xmp || xmp.find("</rdf:RDF>") == std::string::npos) {
        if (opt.strict) {
            fail(
                FX_PDF_NO_XMP,
                "source PDF has no usable XMP /Metadata stream, so it is not PDF/A-3 "
                "(options.strict)");
        }
        warnings.push_back(
            "source PDF has no usable XMP /Metadata stream, which strongly suggests it is not "
            "PDF/A-3; a new XMP packet declaring pdfaid:part 3 was created");
        xmp = build_new_xmp(xi);
    } else {
        if (xmp.find(xi.xmpNamespace) != std::string::npos) {
            fail(
                FX_PDF_ALREADY_FACTURX,
                "source PDF already carries a Factur-X/ZUGFeRD XMP block for namespace " +
                    xi.xmpNamespace);
        }
        std::string part = read_pdfaid_part(xmp);
        if (part.empty()) {
            if (opt.strict) {
                fail(
                    FX_PDF_NOT_PDFA3,
                    "source XMP does not declare pdfaid:part, so it is not PDF/A (options.strict)");
            }
            warnings.push_back("source XMP does not declare pdfaid:part; it may not be PDF/A");
        } else if (part != "3") {
            if (opt.strict) {
                fail(
                    FX_PDF_NOT_PDFA3,
                    "source XMP declares pdfaid:part " + part +
                        "; Factur-X requires PDF/A-3 (options.strict)");
            }
            if (opt.forcePdfAPart3) {
                xmp = force_pdfaid_part3(xmp);
                warnings.push_back(
                    "source XMP declared pdfaid:part " + part +
                    " and was rewritten to 3 because options.forcePdfAPart3 is True; this does "
                    "not by itself make the document PDF/A-3 conformant");
            } else {
                warnings.push_back(
                    "source XMP declares pdfaid:part " + part +
                    "; Factur-X requires PDF/A-3 and the output will not validate as such");
            }
        }
        std::string extended = extend_xmp(xmp, xi, warnings);
        if (extended.empty()) {
            fail(FX_BACKEND_ERROR, "the existing XMP packet could not be extended");
        }
        xmp = extended;
    }

    /* --- 5. embed ----------------------------------------------------- */
    std::string now = QUtil::qpdf_time_to_pdf_time(QUtil::get_current_qpdf_time());
    QPDFEmbeddedFileDocumentHelper efdh(pdf);

    attach(pdf, efdh, xi.name, xi.data, "text/xml", xi.relationship, xi.description, now);
    for (Attachment const& a: attachments) {
        attach(pdf, efdh, a.name, a.data, a.type, a.relationship, a.description, now);
    }
    efdh.validate();

    /* Write the metadata stream unfiltered, as PDF/A requires. */
    if (meta.isStream()) {
        meta.replaceStreamData(xmp, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
        meta.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/Metadata"));
        meta.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/XML"));
    } else {
        QPDFObjectHandle dict = QPDFObjectHandle::newDictionary();
        dict.replaceKey("/Type", QPDFObjectHandle::newName("/Metadata"));
        dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/XML"));
        QPDFObjectHandle stream = QPDFObjectHandle::newStream(&pdf, xmp);
        stream.getDict().replaceKey("/Type", dict.getKey("/Type"));
        stream.getDict().replaceKey("/Subtype", dict.getKey("/Subtype"));
        root.replaceKey("/Metadata", pdf.makeIndirectObject(stream));
    }

    /* --- 6. atomic write ---------------------------------------------- */
    std::string dir = directory_of(out_path);
    std::string tmp_path;
    {
        std::ostringstream o;
        o << dir << "/.facturx-" << QUtil::get_current_qpdf_time().second << "-"
          << static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&pdf) & 0xFFFFFFu) << ".tmp";
        tmp_path = o.str();
    }
    {
        FILE* probe = QUtil::safe_fopen(tmp_path.c_str(), "wb");
        if (probe == nullptr) {
            fail(
                FX_OUT_DIR_UNWRITABLE,
                "the directory of pdfPathOut does not exist or is not writable: " + dir);
        }
        fclose(probe);
    }

    try {
        QPDFWriter w(pdf);
        w.setOutputFilename(tmp_path.c_str());
        /* preserve => never recompress existing streams and never compress the
           XMP metadata stream, which PDF/A requires to be unfiltered. */
        w.setStreamDataMode(qpdf_s_preserve);
        w.setObjectStreamMode(qpdf_o_preserve);
        w.setNewlineBeforeEndstream(true);
        w.setMinimumPDFVersion("1.7");
        w.write();
    } catch (QPDFExc const& e) {
        QUtil::remove_file(tmp_path.c_str());
        fail(FX_BACKEND_ERROR, std::string("qpdf failed to write the output: ") + e.what());
    } catch (std::exception const& e) {
        QUtil::remove_file(tmp_path.c_str());
        fail(FX_OUT_TEMP_FAILED, std::string("the temporary output could not be written: ") + e.what());
    }

    /* Release the input file handle before replacing it (required on Windows
       for the in-place case). */
    pdf.closeInputSource();

    try {
        QUtil::rename_file(tmp_path.c_str(), out_path.c_str());
    } catch (std::exception const& e) {
        QUtil::remove_file(tmp_path.c_str());
        fail(
            FX_OUT_RENAME_FAILED,
            std::string("the temporary output could not be moved into place: ") + e.what());
    }

    result.code = FX_OK;
    result.message.clear();
}

PA_ObjectRef
make_status(Result const& r)
{
    PA_ObjectRef status = PA_CreateObject();
    object_set_bool(status, "success", r.code == FX_OK);
    object_set_long(status, "errorCode", static_cast<PA_long32>(r.code));
    object_set_text(status, "errorMessage", r.message);

    PA_CollectionRef warnings = PA_CreateCollection();
    for (size_t i = 0; i < r.warnings.size(); ++i) {
        std::vector<PA_Unichar> w = utf8_to_utf16(r.warnings[i]);
        PA_Unistring u = PA_CreateUnistring(w.data());
        PA_Variable v;
        std::memset(&v, 0, sizeof(v));
        PA_SetStringVariable(&v, &u);
        PA_SetCollectionElement(warnings, static_cast<PA_long32>(i), v);
        PA_ClearVariable(&v);
    }
    /*
     * PA_SetObjectProperty copies the value into the object and
     * object_set_collection then clears the local variable, which is the
     * one and only release of this collection. Calling PA_DisposeCollection
     * on top of that over-releases it and leaves the status object with a
     * dangling property.
     */
    object_set_collection(status, "warnings", warnings);
    return status;
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* 4D entry points                                                    */
/* ------------------------------------------------------------------ */

void
PluginMain(PA_long32 selector, PA_PluginParameters params)
{
    try {
        switch (selector) {
        case 1:
            PDFA_Embed_FacturX(params);
            break;
        default:
            break;
        }
    } catch (...) {
        /* Nothing may cross the plugin/4D boundary. */
    }
}

static void
PDFA_Embed_FacturX(PA_PluginParameters params)
{
    Result result;
    try {
        run(params, result);
    } catch (FxError const& e) {
        result.code = e.code();
        result.message = e.message();
    } catch (QPDFExc const& e) {
        result.code = FX_BACKEND_ERROR;
        result.message = std::string("qpdf error: ") + e.what();
    } catch (std::bad_alloc const&) {
        result.code = FX_UNEXPECTED_EXCEPTION;
        result.message = "out of memory";
    } catch (std::exception const& e) {
        result.code = FX_UNEXPECTED_EXCEPTION;
        result.message = std::string("unexpected error: ") + e.what();
    } catch (...) {
        result.code = FX_UNKNOWN_FATAL;
        result.message = "unknown fatal error";
    }

    PA_ObjectRef status = nullptr;
    try {
        status = make_status(result);
    } catch (...) {
        status = nullptr;
    }
    if (status != nullptr) {
        /* PA_ReturnObject takes ownership; do not dispose. */
        PA_ReturnObject(params, status);
    }
}
