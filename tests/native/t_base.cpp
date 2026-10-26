#include "../../src/base.h"
#include <assert.h>
using namespace vm2b;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int fails = 0;
int main() {
    Str s = "héllo"; s += " world"; CHECK(s.size() == 12); CHECK(s.find("wor") == 7);
    CHECK(Str::fmt("%d-%s", 5, "x") == "5-x");
    Str big = Str::fmt("%0500d", 7); CHECK(big.size() == 500);
    CHECK(Str("a,b,,c").replace_all(",", ";") == "a;b;;c");
    Vec<Str> v; for (int i = 0; i < 100; ++i) v.push_back(Str::fmt("s%d", i)); CHECK(v.size() == 100 && v[99] == "s99");
    v.erase(0); CHECK(v[0] == "s1"); v.insert(0, "zz"); CHECK(v[0] == "zz" && v.size() == 100);
    Vec<Str> w = v; CHECK(w.size() == 100 && w[50] == v[50]);
    Json j; Str err;
    const char* t = "{\"a\":[1,2.5,-3,true,null,\"x\\u00e9\\ud83d\\ude00\\n\"],\"b\":{\"c\":\"d\"},\"e\":9223372036854775807}";
    CHECK(Json::parse(t, strlen(t), j, err));
    CHECK(j.get("a").size() == 6); CHECK(j.get("a").at(1).num() == 2.5); CHECK(j.get("a").at(2).integer() == -3);
    CHECK(j.get("a").at(3).boolean()); CHECK(j.get("a").at(4).is_null());
    CHECK(j.get("a").at(5).str() == "x\xc3\xa9\xf0\x9f\x98\x80\n"); CHECK(j.get("b").get("c").str() == "d");
    CHECK(j.get("e").integer() == 9223372036854775807LL); CHECK(j.get("zz").is_null());
    Json k; CHECK(Json::parse(j.dump(), k, err)); CHECK(k == j);
    Json k2; CHECK(Json::parse(j.dump(true), k2, err)); CHECK(k2 == j);
    const char* bad[] = {"", "{", "[1,]", "{\"a\":}", "01", "\"\\x\"", "[1] x", "\"\\ud800\"", "nul", "{\"a\" 1}", "-", "1.", "\"a\nb\""};
    for (auto b : bad) { Json q; CHECK(!Json::parse(b, strlen(b), q, err)); }
    Str deep; for (int i = 0; i < 500; ++i) deep += '['; Json q; CHECK(!Json::parse(deep, q, err));
    Bytes b; CHECK(Base64Decode("Zm9vYmFy", 8, b) && b.size() == 6 && memcmp(b.data(), "foobar", 6) == 0);
    CHECK(Base64Encode("fo", 2) == "Zm8="); CHECK(Base64Encode("", 0) == ""); CHECK(!Base64Decode("Zm9v!", 5, b));
    CHECK(HexEncode("\x01\xab", 2) == "01ab"); CHECK(HexDecode("01AB", 4, b) && b.size() == 2 && b[1] == 0xab); CHECK(!HexDecode("0", 1, b));
    CHECK(Utf8Valid("h\xc3\xa9", 3)); CHECK(!Utf8Valid("\xc0\x80", 2)); CHECK(!Utf8Valid("\xed\xa0\x80", 3));
    CHECK(FormatSize(1536) == "1.5 KB"); CHECK(FormatDuration(3725) == "1h 02m 05s");
    Str e; CHECK(WriteWholeFile("/tmp/vm2b_t.json", "hello", 5, e)); Bytes r; CHECK(ReadWholeFile("/tmp/vm2b_t.json", r, e) && r.size() == 5);
    printf(fails ? "FAILED %d\n" : "base ok\n", fails); return fails != 0;
}
