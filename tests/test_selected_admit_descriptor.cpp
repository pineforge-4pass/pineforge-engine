#include "../tools/selected_admit/descriptor.hpp"
#include "../tools/selected_admit/result.hpp"

#include <cassert>
#include <climits>
#include <iostream>
#include <string>

namespace {
namespace sa = pineforge::selected_admit;
using pineforge::live::Json;
constexpr char kIdentity[] = "abcdef0123456789abcdef0123456789abcdef01";

Json request() {
    return pineforge::live::parse_json(
        R"({"schema":"pineforge-selected-capacity-request/v1",)"
        R"("expect":{"git_sha":"abcdef0123456789abcdef0123456789abcdef01"},)"
        R"("identity":{"run_id":"run","attempt_id":"attempt"},)"
        R"("bounds":{},"limits":{"max_record_bytes":"4096"},)"
        R"("streams":[{"id":"s","role":"source","kind":"file","fd":"7",)"
        R"("bytes":"999","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})");
}

Json calendar() {
    return pineforge::live::parse_json(
        R"({"start_ms":"0","end_ms":"60000","fed_start_ms":"-60000",)"
        R"("preroll_bars":"0","input_tf":"1","script_tf":"1",)"
        R"("chart_timezone":"","engine_timezone":"","session":"","feed_tolerant":true})");
}

sa::DecodeStatus decode(const Json& j, std::string_view identity = kIdentity) {
    sa::Descriptor out;
    std::string why;
    return sa::decode_descriptor(j.dump(), sa::Stage::Selected, identity, out, why);
}

void version_and_compiled_identity() {
    for (const auto& value : {"pineforge-selected-capacity-request/v2",
                              "pineforge-selected-capacity-request/v0", "",
                              "PINEFORGE-selected-capacity-request/v1"}) {
        auto j = request();
        j.members["schema"] = Json::string(value);
        j.members.erase("bounds");
        assert(decode(j) == sa::DecodeStatus::UnknownVersion);
    }
    auto j = request();
    j.members["schema"] = Json::number("1");
    assert(decode(j) == sa::DecodeStatus::UnknownVersion);
    j.members.erase("schema");
    assert(decode(j) == sa::DecodeStatus::UnknownVersion);
    j = request();
    j.members["expect"].members["git_sha"] = Json::string("123456789abc");
    assert(decode(j) == sa::DecodeStatus::HelperMismatch);
    j.members["expect"].members["git_sha"] = Json::string(std::string(kIdentity, 12));
    assert(decode(j) == sa::DecodeStatus::Ok);
    for (const auto& weak : {"unknown", "abcdef0", "ABCDEF0123456789abcdef0123456789abcdef01"}) {
        assert(decode(j, weak) == sa::DecodeStatus::HelperMismatch);
        auto equal = request();
        equal.members["expect"].members["git_sha"] = Json::string(weak);
        assert(decode(equal, weak) == sa::DecodeStatus::Invalid);
    }
}

void required_members() {
    for (const auto& key : {"expect", "identity", "bounds", "limits", "streams"}) {
        auto j = request();
        j.members.erase(key);
        assert(decode(j) == sa::DecodeStatus::Invalid);
    }
    for (const auto& key : {"id", "role", "kind", "fd", "bytes", "sha256"}) {
        auto j = request();
        j.members["streams"].items[0].members.erase(key);
        assert(decode(j) == sa::DecodeStatus::Invalid);
    }
    for (const auto& key : {"run_id", "attempt_id"}) {
        auto j = request();
        j.members["identity"].members.erase(key);
        assert(decode(j) == sa::DecodeStatus::Invalid);
    }
    auto j = request();
    j.members["expect"].members.clear();
    assert(decode(j) == sa::DecodeStatus::Invalid);
    j = request();
    j.members["limits"].members.clear();
    assert(decode(j) == sa::DecodeStatus::Invalid);
    j = request();
    j.members["bounds"].members["script_bars"] = Json::string("1");
    assert(decode(j) == sa::DecodeStatus::Invalid);
    j.members["calendar"] = calendar();
    assert(decode(j) == sa::DecodeStatus::Ok);
    for (const auto& [key, value] : calendar().members) {
        auto missing = j;
        missing.members["calendar"].members.erase(key);
        assert(decode(missing) == sa::DecodeStatus::Invalid);
        (void)value;
    }
    auto pipe = request();
    auto& stream = pipe.members["streams"].items[0];
    stream.members["kind"] = Json::string("pipe");
    stream.members.erase("bytes");
    stream.members.erase("sha256");
    stream.members["forward_fd"] = Json::string("8");
    sa::Descriptor out;
    std::string why;
    assert(sa::decode_descriptor(pipe.dump(), sa::Stage::Source, kIdentity, out, why)
           == sa::DecodeStatus::Ok);
    stream.members.erase("forward_fd");
    assert(sa::decode_descriptor(pipe.dump(), sa::Stage::Source, kIdentity, out, why)
           == sa::DecodeStatus::Invalid);
}

void ambiguous_and_bounded_documents() {
    for (const auto& value : {"01", "-1", "18446744073709551616"}) {
        auto j = request();
        j.members["bounds"].members["bytes"] = Json::string(value);
        assert(decode(j) == sa::DecodeStatus::Invalid);
    }
    for (const auto& value : {Json::number("0"), Json{}}) {
        auto j = request();
        j.members["bounds"].members["bytes"] = value;
        assert(decode(j) == sa::DecodeStatus::Invalid);
    }
    auto j = request();
    j.members["bounds"].members["bytes"] = Json::string("18446744073709551615");
    assert(decode(j) == sa::DecodeStatus::Ok);
    for (const auto& value : {std::string(11, 'a'), std::string(41, 'a'),
                              std::string("Abcdef012345")}) {
        j = request();
        j.members["expect"].members["git_sha"] = Json::string(value);
        assert(decode(j) == sa::DecodeStatus::Invalid);
    }
    j = request();
    j.members["immutable"] = Json::boolean(true);
    assert(decode(j) == sa::DecodeStatus::Invalid);
    sa::Descriptor out;
    std::string why;
    const auto raw = request().dump();
    for (const auto& bad : {raw + "x", raw.substr(0, raw.size() - 1),
                            std::string("{\"schema\":\"x\",\"schema\":\"y\"}")}) {
        assert(sa::decode_descriptor(bad, sa::Stage::Selected, kIdentity, out, why)
               == sa::DecodeStatus::Invalid);
    }
    assert(sa::decode_descriptor(std::string(sa::kDescriptorBytes + 1, ' '),
                                sa::Stage::Selected, kIdentity, out, why)
           == sa::DecodeStatus::TooLarge);
    for (const auto& cap : {"0", "1048577"}) {
        j = request();
        j.members["limits"].members["max_record_bytes"] = Json::string(cap);
        assert(decode(j) == sa::DecodeStatus::Invalid);
    }
    for (const auto count : {0U, 256U, 257U}) {
        j = request();
        const auto seed = j.members["streams"].items[0];
        j.members["streams"].items.clear();
        for (unsigned i = 0; i < count; ++i) {
            auto stream = seed;
            stream.members["id"] = Json::string("stream-" + std::to_string(i));
            stream.members["fd"] = Json::string(std::to_string(7 + i));
            j.members["streams"].items.push_back(std::move(stream));
        }
        assert(decode(j) == (count == 256 ? sa::DecodeStatus::Ok : sa::DecodeStatus::Invalid));
    }
}

void absent_zero_and_descriptor_numbers() {
    sa::Descriptor out;
    std::string why;
    auto j = request();
    assert(sa::decode_descriptor(j.dump(), sa::Stage::Selected, kIdentity, out, why)
           == sa::DecodeStatus::Ok);
    assert(!sa::any_requested(out.bounds));
    j.members["bounds"].members["source_rows"] = Json::string("0");
    assert(sa::decode_descriptor(j.dump(), sa::Stage::Selected, kIdentity, out, why)
           == sa::DecodeStatus::Ok);
    assert(out.bounds.source_rows.present && out.bounds.source_rows.value == 0);
    for (const auto& fd : {"0", "1", "2", "2147483648"}) {
        auto invalid = request();
        invalid.members["streams"].items[0].members["fd"] = Json::string(fd);
        assert(decode(invalid) == sa::DecodeStatus::Invalid);
    }
    for (int fd : {3, 4, 5}) {
        j = request();
        j.members["streams"].items[0].members["fd"] = Json::string(std::to_string(fd));
        assert(sa::decode_descriptor(j.dump(), sa::Stage::Selected, kIdentity, out, why)
               == sa::DecodeStatus::Ok);
        assert(sa::descriptor_fds_distinct(out, fd + 10));
        assert(!sa::descriptor_fds_distinct(out, fd));
        assert(sa::decode_descriptor(j.dump(), sa::Stage::Selected, kIdentity, out, why, fd)
               == sa::DecodeStatus::Invalid);
    }
    j = request();
    j.members["streams"].items.push_back(j.members["streams"].items[0]);
    j.members["streams"].items[1].members["id"] = Json::string("other");
    assert(decode(j) == sa::DecodeStatus::Invalid);
    j = request();
    j.members["streams"].items[0].members["role"] = Json::string("other");
    assert(decode(j) == sa::DecodeStatus::Invalid);
}
}  // namespace

int main() {
    version_and_compiled_identity();
    required_members();
    ambiguous_and_bounded_documents();
    absent_zero_and_descriptor_numbers();
    std::cout << "descriptor scenarios: 4 complete\n";
}
