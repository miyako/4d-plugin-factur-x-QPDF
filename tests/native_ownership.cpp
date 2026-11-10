#include "facturx-4dplugin.cpp"
#include "PrivateTypes.h"
#include "EntryPoints.h"
#include <CoreFoundation/CoreFoundation.h>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <unordered_map>

namespace host {
struct Node {
    int refs = 1;
    std::map<std::u16string, PA_Variable> properties;
    std::vector<PA_Variable> elements;
};
std::set<Node*> nodes;
std::set<PA_Unichar*> strings;
std::unordered_map<PA_Handle, size_t> handles;
bool fail_property = false, fail_element = false, null_empty = false;
int invalid_clears = 0;

PA_Variable undefined()
{
    PA_Variable v{};
    v.fType = eVK_Undefined;
    return v;
}

std::u16string text(PA_Unistring const& u)
{
    if (!u.fString) {
        assert(u.fLength == 0);
        return {};
    }
    auto* data = u.fString;
    assert(strings.count(data));
    assert(u.fString[u.fLength] == 0);
    return {reinterpret_cast<char16_t*>(u.fString), static_cast<size_t>(u.fLength)};
}

PA_Unistring allocate(std::u16string const& s)
{
    PA_Unistring u{};
    if (s.empty() && null_empty) return u;
    u.fString = static_cast<PA_Unichar*>(std::calloc(s.size() + 1, sizeof(PA_Unichar)));
    assert(u.fString);
    std::memcpy(u.fString, s.data(), s.size() * sizeof(PA_Unichar));
    u.fLength = static_cast<PA_long32>(s.size());
    auto* data = u.fString;
    strings.insert(data);
    return u;
}

void dispose(PA_Unistring u)
{
    if (u.fString) {
        auto* data = u.fString;
        assert(strings.erase(data) == 1);
        std::free(u.fString);
    }
}

Node* node(void* p)
{
    auto* n = static_cast<Node*>(p);
    assert(nodes.count(n));
    return n;
}

void clear(PA_Variable& v);

void release(Node* n)
{
    if (!n) return;
    assert(nodes.count(n));
    if (--n->refs == 0) {
        for (auto& p: n->properties) clear(p.second);
        for (auto& e: n->elements) clear(e);
        nodes.erase(n);
        delete n;
    }
}

PA_Variable copy(PA_Variable v)
{
    if (v.fType == eVK_Unistring) v.uValue.fString = allocate(text(v.uValue.fString));
    if (v.fType == eVK_Object && v.uValue.fObject) ++node(v.uValue.fObject)->refs;
    if (v.fType == eVK_Collection && v.uValue.fCollection) ++node(v.uValue.fCollection)->refs;
    return v;
}

void clear(PA_Variable& v)
{
    if (v.fFiller == 0x5a) {
        ++invalid_clears;
        return;
    }
    switch (v.fType) {
    case 0:
        ++invalid_clears;
        return;
    case eVK_Unistring: dispose(v.uValue.fString); break;
    case eVK_Object: release(static_cast<Node*>(v.uValue.fObject)); break;
    case eVK_Collection: release(static_cast<Node*>(v.uValue.fCollection)); break;
    case eVK_ArrayUnicode: {
        auto* p = reinterpret_cast<PA_Unistring*>(*v.uValue.fArray.fData);
        for (int i = 0; i <= v.uValue.fArray.fNbElements; ++i) dispose(p[i]);
        PA_DisposeHandle(v.uValue.fArray.fData);
        break;
    }
    default: assert(v.fType == eVK_Undefined || v.fType == eVK_Longint || v.fType == eVK_Boolean);
    }
    v = undefined();
}

void callback(short selector, EngineBlock* eb)
{
    switch (selector) {
    case EX_CREATE_UNISTRING:
        eb->fUniString1 = allocate(reinterpret_cast<char16_t*>(eb->fHandle));
        break;
    case EX_DISPOSE_UNISTRING: dispose(eb->fUniString1); break;
    case EX_SET_UNISTRING:
        dispose(eb->fUniString1);
        eb->fUniString1 = allocate(reinterpret_cast<char16_t*>(eb->fHandle));
        break;
    case EX_VARIABLE_TO_STRING: {
        auto& v = *reinterpret_cast<PA_Variable*>(eb->fHandle);
        assert(v.fType == eVK_Unistring);
        text(v.uValue.fString);
        eb->fUniString1 = v.uValue.fString;
        break;
    }
    case EX_CLEAR_VARIABLE: clear(*static_cast<PA_Variable*>(eb->fPtr1)); break;
    case EX_CONVERT_CHARSET_TO_CHARSET: {
        auto encoding = [](sLONG_PTR c) {
            assert(c == eVTC_UTF_8 || c == eVTC_UTF_16);
            return c == eVTC_UTF_8 ? kCFStringEncodingUTF8 : kCFStringEncodingUTF16LE;
        };
        auto s = CFStringCreateWithBytes(nullptr, static_cast<UInt8*>(eb->fPtr1),
            eb->fParam1, encoding(eb->fParam2), false);
        assert(s);
        CFIndex bytes = 0;
        auto count = CFStringGetLength(s);
        assert(CFStringGetBytes(s, CFRangeMake(0, count), encoding(eb->fParam4), 0,
            false, static_cast<UInt8*>(eb->fPtr2), eb->fParam3, &bytes) == count);
        assert(bytes <= eb->fParam3);
        eb->fParam3 = bytes;
        CFRelease(s);
        break;
    }
    case EX_GET_INFORMATION: eb->fParam2 = 0x00002101; break;
    case EX_HANDLE_MANAGER: {
        auto h = eb->fHandle;
        switch (eb->fParam1) {
        case 1:
            h = static_cast<PA_Handle>(std::malloc(sizeof(char*)));
            *h = static_cast<char*>(std::calloc(eb->fParam2 > 0 ? eb->fParam2 : 1, 1));
            handles[h] = eb->fParam2;
            eb->fHandle = h;
            break;
        case 2:
            assert(handles.erase(h) == 1);
            std::free(*h);
            std::free(h);
            break;
        case 3:
            assert(handles.count(h));
            *h = static_cast<char*>(std::realloc(*h, eb->fParam2));
            handles[h] = eb->fParam2;
            break;
        case 4: eb->fParam2 = handles.at(h); break;
        case 5: eb->fParam3 = reinterpret_cast<sLONG_PTR>(*h); break;
        case 6: break;
        default: std::abort();
        }
        break;
    }
    case EX_EXECUTE_COMMAND_BY_ID: {
        assert(eb->fParam1 == 1471 || eb->fParam1 == 1472);
        auto* n = new Node;
        nodes.insert(n);
        auto& v = *static_cast<PA_Variable*>(eb->fPtr2);
        v = undefined();
        if (eb->fParam1 == 1471) PA_SetObjectVariable(&v, n);
        else PA_SetCollectionVariable(&v, n);
        break;
    }
    case EX_GET_OBJ_VALUE: {
        auto& out = *static_cast<PA_Variable*>(eb->fPtr2);
        bool collection = eb->fShort == eVK_Collection;
        if ((!collection && fail_property) || (collection && eb->fLongint >= 0 && fail_element)) {
            std::memset(&out, 0xa5, sizeof(out));
            out.fType = eVK_Longint;
            out.fFiller = 0x5a;
            eb->fError = -1234;
            break;
        }
        Node* n = node(eb->fPtr1);
        if (collection && eb->fLongint == -1) {
            assert(text(eb->fUniString1) == u"length");
            out = undefined();
            PA_SetLongintVariable(&out, static_cast<PA_long32>(n->elements.size()));
        } else if (collection) out = copy(n->elements.at(eb->fLongint));
        else {
            auto it = n->properties.find(text(eb->fUniString1));
            out = it == n->properties.end() ? undefined() : copy(it->second);
        }
        break;
    }
    case EX_SET_OBJ_VALUE: {
        Node* n = node(eb->fPtr1);
        PA_Variable* destination;
        if (eb->fShort == eVK_Collection) {
            while (n->elements.size() <= static_cast<size_t>(eb->fLongint))
                n->elements.push_back(undefined());
            destination = &n->elements[eb->fLongint];
        } else {
            auto inserted = n->properties.emplace(text(eb->fUniString1), undefined());
            destination = &inserted.first->second;
        }
        clear(*destination);
        *destination = copy(*static_cast<PA_Variable*>(eb->fPtr2));
        break;
    }
    default:
        std::cerr << "unexpected engine selector " << selector << '\n';
        std::abort();
    }
}

void balanced()
{
    assert(strings.empty());
    assert(handles.empty());
    assert(nodes.empty());
    assert(invalid_clears == 0);
}

void failed_getter(bool property)
{
    auto obj = PA_CreateObject();
    auto col = PA_CreateCollection();
    PA_Variable v = undefined();
    PA_SetObjectVariable(&v, obj);
    PA_SetCollectionElement(col, 0, v);
    fail_property = property;
    fail_element = !property;
    bool caught = false;
    try {
        if (property) {
            PAVar result(object_property(obj, "path"));
        } else {
            parse_attachments(col, XmlInfo{});
        }
    } catch (FxError const& e) {
        caught = e.code() == FX_BACKEND_ERROR &&
            e.message().find("-1234") != std::string::npos &&
            e.message().find(property ? "path" : "attachments[0]") != std::string::npos;
    }
    fail_property = fail_element = false;
    PA_DisposeCollection(col);
    PA_DisposeObject(obj);
    if (!caught || invalid_clears) {
        std::cerr << "FAIL " << (property ? "failed-property-output" : "failed-element-output")
                  << ": backend error=" << caught << ", invalid clears=" << invalid_clears << '\n';
        std::exit(1);
    }
}

void successful_paths()
{
    std::vector<std::string> fixtures = {"ASCII", "\xe6\x97\xa5\xe6\x9c\xac",
        "\xf0\x9f\x98\x80", ""};
    std::vector<std::u16string> expected = {u"ASCII", u"\u65e5\u672c", u"\U0001f600", u""};
    for (bool null_representation: {false, true}) {
        null_empty = null_representation;
        for (size_t i = 0; i < fixtures.size(); ++i) {
            auto units = utf8_to_utf16(fixtures[i]);
            assert(units.size() == expected[i].size() + 1 && units.back() == 0);
            assert(std::equal(expected[i].begin(), expected[i].end(), units.begin()));
            {
                UStr s(fixtures[i]);
                assert(text(*s.get()) == expected[i]);
                assert(utf16_to_utf8(*s.get()) == fixtures[i]);
            }
            auto obj = PA_CreateObject();
            object_set_text(obj, "text", fixtures[i]);
            std::string out;
            assert(object_get_text(obj, "text", out, "test") && out == fixtures[i]);
            {
                PAVar owned(object_property(obj, "text"));
                object_set_text(obj, "text", "replacement");
                assert(utf16_to_utf8(PA_GetStringVariable(owned.get())) == fixtures[i]);
            }
            assert(!object_get_text(obj, "absent", out, "test"));
            object_set_bool(obj, "flag", true);
            assert(object_get_bool(obj, "flag", false, "test"));
            bool mismatch = false;
            try { object_get_bool(obj, "text", false, "test"); }
            catch (FxError const& e) { mismatch = e.code() == FX_PROPERTY_WRONG_TYPE; }
            assert(mismatch);
            PA_DisposeObject(obj);
        }
        Result r;
        r.message = fixtures[2];
        r.warnings = fixtures;
        auto status = make_status(r);
        {
            PAVar warnings(object_property(status, "warnings"));
            auto col = PA_GetCollectionVariable(warnings.get());
            PA_DisposeObject(status);
            for (size_t i = 0; i < fixtures.size(); ++i) {
                PAVar item(PA_GetCollectionElement(col, static_cast<PA_long32>(i)));
                assert(PA_GetLastError() == eER_NoErr);
                assert(utf16_to_utf8(PA_GetStringVariable(item.get())) == fixtures[i]);
            }
        }
        auto col = PA_CreateCollection();
        auto obj = PA_CreateObject();
        PA_Variable v = undefined();
        PA_SetObjectVariable(&v, obj);
        PA_SetCollectionElement(col, 0, v);
        PA_DisposeObject(obj);
        {
            PAVar item(PA_GetCollectionElement(col, 0));
            PA_DisposeCollection(col);
            auto attachment = PA_GetObjectVariable(item.get());
            object_set_text(attachment, "description", fixtures[1]);
            std::string fixture = std::string(__FILE__);
            fixture = fixture.substr(0, fixture.rfind('/')) +
                "/../facturx/facturx-test/Resources/notes.txt";
            object_set_text(attachment, "path", fixture);
            object_set_text(attachment, "name", "notes.txt");
            object_set_text(attachment, "type", "text/plain");
            col = PA_CreateCollection();
            PA_SetCollectionElement(col, 0, item.get());
            auto attachments = parse_attachments(col, XmlInfo{});
            assert(attachments.size() == 1 && attachments[0].description == fixtures[1]);
            assert(attachments[0].data == read_whole_file(fixture, FX_ATTACHMENT_UNREADABLE, "test"));
            PA_DisposeCollection(col);
        }
        col = PA_CreateCollection();
        v = undefined();
        auto units = utf8_to_utf16(fixtures[2]);
        PA_Unistring u = PA_CreateUnistring(units.data());
        PA_SetStringVariable(&v, &u);
        PA_SetCollectionElement(col, 0, v);
        PA_ClearVariable(&v);
        bool mismatch = false;
        size_t before = strings.size();
        try { parse_attachments(col, XmlInfo{}); }
        catch (FxError const& e) { mismatch = e.code() == FX_ATTACHMENT_NOT_OBJECT; }
        assert(mismatch && strings.size() == before);
        PA_DisposeCollection(col);

        auto array = PA_CreateVariable(eVK_ArrayUnicode);
        PA_ResizeArray(&array, 2);
        for (int i = 0; i <= 2; ++i) {
            u = PA_CreateUnistring(units.data());
            PA_SetStringInArray(array, i, &u);
        }
        auto borrowed = PA_GetStringInArray(array, 1);
        auto independent = PA_CreateUnistring(borrowed.fString);
        assert(independent.fString != borrowed.fString);
        PA_SetStringInArray(array, 1, &independent);
        assert(text(PA_GetStringInArray(array, 1)) == expected[2]);
        u = PA_CreateUnistring(units.data());
        for (int index: {-1, 3}) {
            PA_SetStringInArray(array, index, &u);
            assert(text(u) == expected[2]);
        }
        auto wrong = undefined();
        PA_SetStringInArray(wrong, 0, &u);
        auto missing_data = array;
        missing_data.uValue.fArray.fData = nullptr;
        PA_SetStringInArray(missing_data, 0, &u);
        PA_SetStringInArray(array, 0, nullptr);
        assert(text(u) == expected[2]);
        PA_DisposeUnistring(&u);
        PA_ResizeArray(&array, 0);
        PA_ClearVariable(&array);
        PA_ObjectRef returned = nullptr;
        PluginBlock parameters{};
        parameters.fResult = &returned;
        auto handed = make_status(Result{});
        PA_ReturnObject(&parameters, handed);
        assert(returned == handed);
        assert(object_get_bool(returned, "success", false, "result"));
        PA_DisposeObject(returned);
        balanced();
    }
}
} // namespace host

int main(int argc, char** argv)
{
    assert(argc == 2);
    gCall4D = host::callback;
    std::string test = argv[1];
    if (test == "failed-property-output") host::failed_getter(true);
    else if (test == "failed-element-output") host::failed_getter(false);
    else if (test == "successful-ownership") host::successful_paths();
    else std::abort();
    host::balanced();
    std::cout << "PASS " << test << '\n';
}
