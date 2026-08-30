/*
 * test_json_helpers.cpp
 *
 * Unit tests for the JSON builder/parser helpers used in src/RackMcpServer.cpp.
 * Compiled standalone — no VCV Rack SDK required.
 *
 * Build & run:
 *   c++ -std=c++17 -o tests/test_json_helpers tests/test_json_helpers.cpp && tests/test_json_helpers
 */

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// ─── Copy of helpers under test (keep in sync with src/RackMcpServer.cpp) ───

static std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out + "\"";
}

static std::string jsonKV(const std::string& k, const std::string& v, bool last = false) {
    return jsonStr(k) + ": " + v + (last ? "" : ", ");
}

static std::string jsonKVs(const std::string& k, const std::string& s, bool last = false) {
    return jsonStr(k) + ": " + jsonStr(s) + (last ? "" : ", ");
}

static std::string ok(const std::string& body) {
    return "{" + jsonKVs("status", "ok") + jsonKV("data", body, true) + "}";
}

static std::string err(const std::string& msg) {
    return "{" + jsonKVs("status", "error") + jsonKVs("message", msg, true) + "}";
}

static std::string parseJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    auto pos = json.find(searchKey);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + searchKey.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static double parseJsonDouble(const std::string& json, const std::string& key, double def = 0.0) {
    std::string searchKey = "\"" + key + "\"";
    auto pos = json.find(searchKey);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos + searchKey.size());
    if (pos == std::string::npos) return def;
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) pos++;
    try { return std::stod(json.substr(pos)); }
    catch (...) { return def; }
}

// ponytail: undoes only the escapes jsonStr() produces (plus \/); no \uXXXX.
static std::string jsonUnescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            out += (n == 'n') ? '\n' : (n == 'r') ? '\r' : (n == 't') ? '\t' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Values of "key": ["a", "b"]; empty if the key or the array is missing.
static std::vector<std::string> parseJsonStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> out;
    size_t k = json.find("\"" + key + "\"");
    if (k == std::string::npos) return out;
    size_t open = json.find('[', k);
    if (open == std::string::npos) return out;
    size_t i = open + 1;
    while (i < json.size() && json[i] != ']') {
        if (json[i] == '"') {
            size_t j = i + 1;
            std::string raw;
            while (j < json.size() && json[j] != '"') {
                if (json[j] == '\\' && j + 1 < json.size()) raw += json[j++];
                raw += json[j++];
            }
            out.push_back(jsonUnescape(raw));
            i = j + 1;
        } else {
            i++;
        }
    }
    return out;
}

struct MenuItemInfo {
    std::vector<std::string> path;   // ["Label"] or ["Label", "Sub label"]
    std::string right;               // Rack's rightText: "✔" for checked, "▸" for submenu, else ""
    bool disabled = false;
};

static std::string pathJson(const std::vector<std::string>& path) {
    std::string s = "[";
    for (size_t j = 0; j < path.size(); j++) { if (j) s += ", "; s += jsonStr(path[j]); }
    return s + "]";
}

static std::string menuItemsJson(long long moduleId, const std::vector<MenuItemInfo>& items) {
    std::string s = "{" + jsonKV("module_id", std::to_string(moduleId)) + "\"items\": [";
    for (size_t i = 0; i < items.size(); i++) {
        if (i) s += ", ";
        s += "{\"path\": " + pathJson(items[i].path) + ", "
           + jsonKVs("right", items[i].right)
           + jsonKV("disabled", items[i].disabled ? "true" : "false", true) + "}";
    }
    return s + "]}";
}

// ─── Minimal test framework ──────────────────────────────────────────────────

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(name, expr) do { \
    if (expr) { \
        printf("  [PASS] %s\n", name); \
        pass_count++; \
    } else { \
        printf("  [FAIL] %s\n", name); \
        fail_count++; \
    } \
} while(0)

// ─── Tests ───────────────────────────────────────────────────────────────────

void test_jsonStr() {
    printf("\njsonStr()\n");
    CHECK("plain string",         jsonStr("hello") == "\"hello\"");
    CHECK("empty string",         jsonStr("") == "\"\"");
    CHECK("escapes double-quote", jsonStr("a\"b") == "\"a\\\"b\"");
    CHECK("escapes backslash",    jsonStr("a\\b") == "\"a\\\\b\"");
    CHECK("escapes newline",      jsonStr("a\nb") == "\"a\\nb\"");
    CHECK("escapes tab",          jsonStr("a\tb") == "\"a\\tb\"");
    CHECK("unicode passthrough",  jsonStr("caf\xc3\xa9") == "\"caf\xc3\xa9\"");
}

void test_jsonKV() {
    printf("\njsonKV()\n");
    CHECK("non-last adds comma",  jsonKV("k", "v") == "\"k\": v, ");
    CHECK("last omits comma",     jsonKV("k", "v", true) == "\"k\": v");
    CHECK("numeric value",        jsonKV("n", "42", true) == "\"n\": 42");
}

void test_jsonKVs() {
    printf("\njsonKVs()\n");
    CHECK("string value quoted",  jsonKVs("k", "hello", true) == "\"k\": \"hello\"");
    CHECK("non-last has comma",   jsonKVs("k", "v") == "\"k\": \"v\", ");
}

void test_ok_wrapper() {
    printf("\nok()\n");
    std::string result = ok("{\"x\":1}");
    CHECK("starts with {",        result.front() == '{');
    CHECK("ends with }",          result.back() == '}');
    CHECK("contains status ok",   result.find("\"status\": \"ok\"") != std::string::npos);
    CHECK("contains data",        result.find("\"data\"") != std::string::npos);
    CHECK("data value present",   result.find("{\"x\":1}") != std::string::npos);
}

void test_err_wrapper() {
    printf("\nerr()\n");
    std::string result = err("something went wrong");
    CHECK("contains status error",  result.find("\"status\": \"error\"") != std::string::npos);
    CHECK("contains message key",   result.find("\"message\"") != std::string::npos);
    CHECK("contains message value", result.find("something went wrong") != std::string::npos);
}

void test_parseJsonString() {
    printf("\nparseJsonString()\n");
    std::string j = R"({"plugin": "VCV", "slug": "VCO-1", "name": "my name"})";
    CHECK("reads plugin",        parseJsonString(j, "plugin") == "VCV");
    CHECK("reads slug",          parseJsonString(j, "slug") == "VCO-1");
    CHECK("reads name",          parseJsonString(j, "name") == "my name");
    CHECK("missing key → empty", parseJsonString(j, "nope") == "");
    CHECK("empty json → empty",  parseJsonString("", "plugin") == "");

    // Embedded spaces in value
    std::string j2 = R"({"path": "/home/user/my patch.vcv"})";
    CHECK("path with space",     parseJsonString(j2, "path") == "/home/user/my patch.vcv");
}

void test_parseJsonDouble() {
    printf("\nparseJsonDouble()\n");
    std::string j = R"({"id": 3, "value": 0.75, "neg": -1.5, "zero": 0})";
    CHECK("integer",             parseJsonDouble(j, "id") == 3.0);
    CHECK("float",               parseJsonDouble(j, "value") == 0.75);
    CHECK("negative",            parseJsonDouble(j, "neg") == -1.5);
    CHECK("zero",                parseJsonDouble(j, "zero") == 0.0);
    CHECK("missing → default",   parseJsonDouble(j, "nope", 99.0) == 99.0);
    CHECK("empty → default",     parseJsonDouble("", "x", -1.0) == -1.0);
}

void test_jsonUnescape() {
    printf("\njsonUnescape()\n");
    CHECK("windows path",     jsonUnescape("C:\\\\Users\\\\Arikl\\\\x.vcv") == "C:\\Users\\Arikl\\x.vcv");
    CHECK("forward slashes",  jsonUnescape("C:\\/Users\\/x.vcv") == "C:/Users/x.vcv");
    CHECK("plain untouched",  jsonUnescape("C:/Users/x.vcv") == "C:/Users/x.vcv");
    CHECK("newline + quote",  jsonUnescape("a\\nb\\\"c") == "a\nb\"c");
    std::string p = "C:\\Users\\my patch.vcv";
    std::string quoted = jsonStr(p);
    CHECK("round-trips jsonStr", jsonUnescape(quoted.substr(1, quoted.size() - 2)) == p);
}

void test_round_trip_ok() {
    printf("\nRound-trip: ok() response parsing\n");
    // Build a typical /status response body and verify it round-trips through
    // our helpers correctly (simulates what the server sends and the client reads).
    std::string body = "{";
    body += jsonKVs("server", "VCV Rack MCP Bridge");
    body += jsonKVs("version", "1.0.0");
    body += jsonKV("sampleRate", "44100");
    body += jsonKV("moduleCount", "3", true);
    body += "}";
    std::string resp = ok(body);

    CHECK("status == ok",         parseJsonString(resp, "status") == "ok");
    // The body is nested — we can't directly parse the inner fields through
    // the simple parser, but we can check the string is present.
    CHECK("body embedded in resp", resp.find("VCV Rack MCP Bridge") != std::string::npos);
    CHECK("sampleRate present",    resp.find("44100") != std::string::npos);
}

void test_parseJsonStringArray() {
    printf("\nparseJsonStringArray()\n");
    auto p = parseJsonStringArray("{\"module_id\": 5, \"path\": [\"Polyphony channels\", \"4\"]}", "path");
    CHECK("array of two",          p.size() == 2 && p[0] == "Polyphony channels" && p[1] == "4");
    CHECK("missing key -> empty",  parseJsonStringArray("{\"x\": [\"a\"]}", "path").empty());
    CHECK("empty array",           parseJsonStringArray("{\"path\": []}", "path").empty());
    auto q = parseJsonStringArray("{\"path\":[\"Say \\\"hi\\\"\"]}", "path");
    CHECK("escaped quote unescaped", q.size() == 1 && q[0] == "Say \"hi\"");
}

void test_menuItemsJson() {
    printf("\nmenuItemsJson()\n");
    std::vector<MenuItemInfo> items = {{{"Initialize"}, "", false}, {{"Polyphony channels", "4"}, "\xe2\x9c\x94", true}};
    CHECK("menu items json",
          menuItemsJson(5, items) ==
          "{\"module_id\": 5, \"items\": [{\"path\": [\"Initialize\"], \"right\": \"\", \"disabled\": false}, "
          "{\"path\": [\"Polyphony channels\", \"4\"], \"right\": \"\xe2\x9c\x94\", \"disabled\": true}]}");
    CHECK("empty items",           menuItemsJson(7, {}) == "{\"module_id\": 7, \"items\": []}");
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main() {
    printf("JSON helper unit tests\n%s", std::string(50, '-').c_str());

    test_jsonStr();
    test_jsonKV();
    test_jsonKVs();
    test_ok_wrapper();
    test_err_wrapper();
    test_parseJsonString();
    test_parseJsonDouble();
    test_jsonUnescape();
    test_round_trip_ok();
    test_parseJsonStringArray();
    test_menuItemsJson();

    printf("\n%s\n", std::string(50, '-').c_str());
    printf("Results: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
