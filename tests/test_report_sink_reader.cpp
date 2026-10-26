// L21C. This test's small stored-column writer is derived from sink-v2.md,
// not from the reader or the engine writer. References are the input rows.
#include "../tools/sink_v2/report_sink_reader.hpp"
#include "sink_v2_reader_fixtures.hpp"
#include "../src/sha256.hpp"
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/version.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

// Count actual C++ allocations during open, including source range buffers.
// Fixtures and the source are constructed before enabling this probe.
namespace allocation_probe {
std::size_t minimum = 0, count = 0;
struct Counter {
    explicit Counter(std::size_t threshold) {
        minimum = threshold; count = 0;
        global_allocation::live_bytes = 0;
        global_allocation::on_recorded = [](std::size_t n) { if (n >= minimum) ++count; };
        global_allocation::recording = true;
    }
    ~Counter() { finish(); }
    std::size_t finish() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        return count;
    }
};
}

namespace {
namespace reader = pineforge::report_sink::v2;
namespace sink = pineforge::report_sink;
namespace fixture = sink_v2_reader_fixture;
using fixture::require;
using fixture::word;
using fixture::i;
using fixture::u;
using fixture::s;
using fixture::equity;
using fixture::trade;
using fixture::population;

class Source final : public reader::RandomAccessSource {
public:
    using reader::RandomAccessSource::readRange;
    explicit Source(std::string bytes) : bytes_(std::move(bytes)) {}
    std::uint64_t size() const override { return bytes_.size(); }
    std::string generation() const override { return generation_; }
    reader::Bytes readRange(std::uint64_t offset, std::size_t n) override {
        require(offset <= bytes_.size() && n <= bytes_.size()-offset, "source range");
        requests.emplace_back(offset,n);
        if (change_during_read) { change_during_read = false; generation_ += "-changed"; }
        const auto begin = bytes_.begin()+static_cast<std::ptrdiff_t>(offset);
        reader::Bytes result(begin,begin+static_cast<std::ptrdiff_t>(n));
        if ((short_read || (short_at && offset==*short_at)) && !result.empty()) result.pop_back();
        return result;
    }
    std::string bytes_, generation_ = "immutable-test-generation";
    bool short_read = false, change_during_read = false;
    std::optional<std::uint64_t> short_at;
    std::vector<std::pair<std::uint64_t,std::size_t>> requests;
};
reader::Reader open(const fixture::Built& b) { return reader::Reader::open(std::make_shared<Source>(b.bytes)); }
void equal(const reader::Record& actual, const fixture::Row& expected) {
    require(actual.tag == expected.tag && actual.values.size() == expected.values.size(), "record shape");
    const auto defs = fixture::definitions(true);
    const auto def = std::find_if(defs.begin(),defs.end(),[&](const auto& d) { return d.tag == expected.tag; });
    require(def != defs.end(), "expected schema");
    for (std::size_t c = 0; c < expected.values.size(); ++c) {
        const auto& v = actual.values[c]; const auto& cell = expected.values[c];
        const auto& type = def->types[c];
        const auto kind = type.compare(0,3,"i64")==0 ? reader::Kind::i64
            : type.compare(0,3,"u64")==0 ? reader::Kind::u64
            : type.compare(0,3,"f64")==0 ? reader::Kind::f64
            : type.compare(0,3,"str")==0 ? reader::Kind::string
            : type=="bool" ? reader::Kind::boolean : reader::Kind::hex64;
        require(v.kind == kind, "decoded kind mismatch");
        require(v.null == fixture::null(cell), "null mismatch");
        if (v.null) continue;
        if (const auto p = std::get_if<std::string>(&cell)) require(v.text == *p, "string mismatch");
        else require(v.word == fixture::numeric(cell), "exact scalar bits mismatch");
    }
}
std::vector<reader::Record> all_records(reader::Reader& r, char tag) {
    std::vector<reader::Record> rows;
    auto cursor = r.iter_type(tag); reader::Record row;
    while (cursor.next(row)) rows.push_back(row);
    return rows;
}
template<class F> void refuses(F&& f, const std::string& label, const std::string& diagnostic = "",
                              std::optional<reader::Failure> failure = std::nullopt) {
    try { f(); }
    catch (const reader::ReaderError& e) {
        require(e.failure() != reader::Failure::cancelled && e.failure() != reader::Failure::unsupported_codec,
                label+": unexpected cancellation/unsupported codec");
        require(!failure || e.failure()==*failure, label+": wrong typed refusal: "+e.what());
        require(diagnostic.empty() || std::string(e.what()).find(diagnostic) != std::string::npos,
                label+": wrong refusal: "+e.what());
        return;
    }
    throw std::runtime_error(label+": incorrectly accepted");
}
void unverified_metadata(reader::Reader& r) {
    const auto status=r.verified();
    require(!status.value && status.scope==reader::VerificationScope::unverified, "premature whole-file state");
    require(r.logical_sha256().scope==status.scope && r.summary().scope==status.scope
        && r.counts().scope==status.scope && r.finite_equity_count().scope==status.scope
        && r.sink_folds().scope==status.scope && r.head().scope==status.scope
        && r.tables().scope==status.scope && r.trade_non_null_count("n").scope==status.scope
        && r.trade_non_null_count("pnl").scope==status.scope && r.profile().scope==status.scope
        && r.generation().scope==status.scope && r.memory_stats().scope==status.scope
        && r.summary_frame_info().scope==status.scope, "accessor lost file-level UNVERIFIED");
}

void test_records_and_columns() {
    fixture::Writer w; w.stored_digest = true;
    const auto input = population(); const auto b = w.encode(input);
    auto source = std::make_shared<Source>(b.bytes);
    auto r = reader::Reader::open(source, {"immutable://fixture",{}});
    require(!r.verified().value, "open cannot verify unseen records");
    require(r.memory_stats().value.live_index_pages == 0, "open loaded descriptor pages");
    for (const auto& request : source->requests) require(request.second != 65536, "open fetched index");
    require(r.head().value.summary == b.summary && r.summary().value.find("\"path\":\"immutable://fixture\"") != std::string::npos,
            "footer metadata preservation/synthesis");
    require(r.head().value.summary.find("\"additive\":{\"kept\":true}") != std::string::npos, "selected additive metadata lost");
    {
        auto merged = r.iter_merged(); reader::Record row; std::size_t n = 0;
        while (merged.next(row)) { require(n < input.size(), "extra merged record"); equal(row,input[n++]); }
        require(n == input.size(), "missing merged record");
    }
    for (std::size_t t = 0; t < 7; ++t) {
        const auto records = all_records(r,fixture::tags[t]);
        require(records.size() == b.rows[t].size(), "per-type count");
        for (std::size_t k = 0; k < records.size(); ++k) { equal(records[k],b.rows[t][k]); require(records[k].ordinal == k,"ordinal"); }
    }
    const auto e = r.descriptor(0,0);
    {
        source->requests.clear();
        const auto columns = r.read_columns(e,{2,0});
        require(columns.scope == reader::VerificationScope::columns && columns.columns[0].id == 2 && columns.columns[1].id == 0,
                "partial-column scope/order");
        require(columns.verified_columns == std::vector<std::uint32_t>({2,0}), "partial-column SHA scope ids");
        for (const auto& request : source->requests)
            require(request.first != e.offset+176+8*e.n, "read_columns fetched unrequested equity payload");
    }
    require(!r.verified().value, "partial read claimed whole-file verification");
    {
        const auto complete = r.read_columns(e,{2,0,1});
        require(complete.scope == reader::VerificationScope::block, "all selected columns omitted block SHA");
        equal(complete.row(0), b.rows[0][0]);
    }
    require(r.verify().scope == reader::VerificationScope::whole_file && r.verified().value, "whole verification");
    const auto stats = r.memory_stats().value;
    require(stats.peak_index_pages <= reader::layout::page_cache, "index cache bound");
    require(stats.peak_decoded_blocks <= reader::layout::stream_count, "merged cursor block bound");
    require(stats.live_decoded_blocks == 0, "blocks leaked on cursor exhaustion");
    require(stats.peak_rank_scratch_bytes <= 65536, "fixed rank verification scratch");
    auto repacked=w; repacked.caps[0]=1; repacked.caps[1]=1; repacked.caps[7]=3;
    auto other=open(repacked.encode(input));
    require(other.verify().scope==reader::VerificationScope::whole_file
        && other.logical_sha256().value==r.logical_sha256().value,"logical identity changed with block boundaries");
    // Empty types, and K with zero fields (a real zero-column block).
    fixture::Writer empty; empty.constants = 0;
    auto er = open(empty.encode({{'K',{}}}));
    require(er.verify().scope == reader::VerificationScope::whole_file, "zero-column K");
    auto k = er.iter_type('K'); reader::Record kr;
    require(k.next(kr) && kr.values.empty() && !k.next(kr), "zero-field K record");
    fixture::Writer minimal; minimal.all = false;
    auto mr = open(minimal.encode({}));
    require(mr.verify().scope == reader::VerificationScope::whole_file, "empty report");
    const auto page = mr.trade_rank_page("pnl",true,99,10);
    require(page.total == 0 && page.page_count == 1 && page.effective_page == 1 && page.from == 0 && page.to == 0
        && page.rows.empty(), "empty rank convention");
    require(mr.equity_summary(10).buckets.empty(), "empty equity envelope");
}

void test_additional_modes() {
    fixture::Writer w; w.all = false; w.caps[0] = 4;
    const std::vector<fixture::Row> input{equity(10,1.25,0.0),equity(20,1.5,0.0),
        equity(30,1.5,0.0),equity(40,2.0,0.0)};
    w.change_column = [](char tag,std::size_t col,fixture::Encoding& e) {
        if (tag != 'E') return;
        if (col == 0) { e.mode=1; e.bytes=fixture::uv(20)+fixture::uv(10)+fixture::uv(2)+fixture::uv(2)+fixture::uv(2); }
        if (col == 1) { e.mode=3; e.bytes=std::string(1,'\2')+fixture::uv(250)+fixture::uv(25)+fixture::uv(2)+fixture::uv(0)+fixture::uv(4); }
        if (col == 2) { e.flags=1; e.bytes=std::string(1,'\0')+e.bytes; } // redundant legal all-zero bitmap
    };
    auto r = open(w.encode(input)); require(r.verify().scope == reader::VerificationScope::whole_file,"DELTA/DECIMAL");
    const auto decoded = all_records(r,'E');
    for (std::size_t j=0;j<input.size();++j) equal(decoded[j],input[j]);
    fixture::Writer d; d.all=false;
    d.change_column=[](char tag,std::size_t col,fixture::Encoding& e) {
        if (tag=='T' && col==17) { e.mode=5; e.bytes=fixture::uv(1)+fixture::uv(5)+"entry"+fixture::uv(0)+fixture::uv(0); }
        if (tag=='T' && col==15) { e.mode=1; e.bytes=fixture::uv(2)+fixture::uv(1)+fixture::uv(2); }
    };
    auto dictionary = open(d.encode({trade(1,1.0),trade(2,2.0)}));
    require(dictionary.verify().scope==reader::VerificationScope::whole_file,"DICT");
    auto rows = all_records(dictionary,'T'); equal(rows[0],trade(1,1.0)); equal(rows[1],trade(2,2.0));
    // Empty DICT is legal for an entirely null optional string column.
    fixture::Writer null_dict;
    null_dict.change_column=[](char tag,std::size_t col,fixture::Encoding& e) {
        if (tag=='V' && col==10) { e.mode=5; e.flags=1; e.bytes=std::string("\1\0",2); }
    };
    auto p=population(); const auto only=p[3];
    auto nr=open(null_dict.encode({only}));
    require(nr.verify().scope==reader::VerificationScope::whole_file,"zero-entry DICT");
    constexpr std::int64_t endpoint=1125899906842623LL;
    fixture::Writer limits; limits.all=false;
    limits.change_column=[](char tag,std::size_t c,fixture::Encoding& e) {
        if(tag=='E'&&c==1) {
            e.mode=3;
            e.bytes=std::string(1,'\14')+fixture::uv(2*UINT64_C(1125899906842623))
                +fixture::uv(1)+fixture::uv(4*UINT64_C(1125899906842623)-1);
        }
    };
    volatile double divisor=1000000000000.0;
    const std::vector<fixture::Row> endpoints{equity(100,static_cast<double>(endpoint)/divisor,0.0),
        equity(200,static_cast<double>(-endpoint)/divisor,0.0)};
    auto lr=open(limits.encode(endpoints));
    require(lr.verify().scope==reader::VerificationScope::whole_file,"inclusive DECIMAL k12/signed endpoints");
    const auto exact=all_records(lr,'E');
    for(std::size_t j=0;j<endpoints.size();++j)equal(exact[j],endpoints[j]);
}

bool same_row(const reader::Record& a,const reader::Record& b) {
    if (a.tag!=b.tag || a.ordinal!=b.ordinal || a.values.size()!=b.values.size()) return false;
    for (std::size_t i=0;i<a.values.size();++i) {
        const auto& x=a.values[i]; const auto& y=b.values[i];
        if (x.kind!=y.kind || x.null!=y.null || (!x.null && (x.word!=y.word || x.text!=y.text))) return false;
    }
    return true;
}
void test_seek_slice_rank(bool compact) {
    fixture::Writer w; w.caps[0]=1; w.caps[1]=10; // equal-time E boundary; byte-triggered T closes
    w.compact=compact;
    auto input=population();
    input[7].values[17]=s(std::string(3000,'a'));
    input[11].values[17]=s(std::string(3000,'b'));
    input[19].values[19]=s(std::string(3500,'c'));
    input[1].values[10]=3.0; input[7].values[10]=-6.0; input[11].values[10]=6.0;
    input[15].values[10]=std::monostate{}; input[19].values[10]=-0.0;
    const auto b=w.encode(input); auto r=open(b);
    require(b.descriptors[1].size()>1 && fixture::get(b.descriptors[1][0],16,4)<w.caps[1],
            "query fixture did not exercise byte-triggered short blocks");
    require(r.verify().scope==reader::VerificationScope::whole_file,"query fixture");
    struct Clock { char tag,key; unsigned column; };
    for (const auto clock : std::vector<Clock>{{'E','A',0},{'T','A',2},{'T','B',3},{'O','A',1},{'O','B',2},
                                              {'V','A',3},{'V','B',4},{'X','A',0}}) {
        const auto all=all_records(r,clock.tag);
        for (const auto& bounds : std::vector<std::array<std::int64_t,2>>{{100,101},{100,100},{160,600},
                                                   {-9007199254740991LL,9007199254740991LL},{999,1000}}) {
            std::vector<reader::Record> expected,actual;
            for (const auto& row:all) {
                const auto& v=row.values[clock.column];
                if (!v.null && v.as_i64()>=bounds[0] && v.as_i64()<bounds[1]) expected.push_back(row);
            }
            auto blocks=r.blocks_for_time_range(clock.tag,clock.key,bounds[0],bounds[1]); reader::BlockEntry e;
            while (blocks.next(e)) {
                auto block=r.read_block(e);
                for (std::uint32_t k=0;k<e.n;++k) {
                    auto row=block.row(k); const auto& v=row.values[clock.column];
                    if (!v.null && v.as_i64()>=bounds[0] && v.as_i64()<bounds[1]) actual.push_back(std::move(row));
                }
            }
            require(actual.size()==expected.size(),"time predicate population");
            for (std::size_t k=0;k<actual.size();++k) require(same_row(actual[k],expected[k]),"time predicate values/order");
            if (clock.tag=='E' && bounds[0]==100 && bounds[1]==101)
                require(actual.size()==2 && actual[0].ordinal==0 && actual[1].ordinal==1,"anchor duplication");
        }
    }
    const auto all=all_records(r,'T');
    for (std::size_t first=1;first<=all.size();++first) for (std::size_t last=first;last<=all.size();++last) {
        auto slice=r.trade_slice(first,last); reader::Record row; auto at=first-1;
        while (slice.next(row)) require(same_row(row,all[at++]),"trade slice exact rows");
        require(at==last,"trade slice endpoint");
    }
    const std::array<std::string_view,6> keys{{"n","exit","pnl","pct","ru","dd"}};
    const unsigned columns[]={0,3,7,8,9,10};
    for (std::size_t k=0;k<keys.size();++k) for (const bool descending : {false,true}) {
        auto sorted=all;
        std::stable_sort(sorted.begin(),sorted.end(),[&](const auto& a,const auto& z) {
            if (k==0) return descending ? a.ordinal>z.ordinal : a.ordinal<z.ordinal;
            const auto& x=a.values[columns[k]]; const auto& y=z.values[columns[k]];
            if (x.null!=y.null) return !x.null;
            if (x.null) return false;
            double xv=k==1?static_cast<double>(x.as_i64()):x.as_f64();
            double yv=k==1?static_cast<double>(y.as_i64()):y.as_f64();
            if (k==5) { xv=-std::abs(xv); yv=-std::abs(yv); }
            return descending?xv>yv:xv<yv;
        });
        const auto finite = std::count_if(all.begin(),all.end(),[&](const auto& row) { return !row.values[columns[k]].null; });
        require(r.trade_non_null_count(keys[k],descending).value==static_cast<std::uint64_t>(finite), "null-last boundary count");
        for (const auto page_number : {-1,1,2,3,99}) {
            const auto p=r.trade_rank_page(keys[k],descending,page_number,2);
            const auto total_pages=(all.size()+1)/2;
            const auto effective=std::min<std::size_t>(std::max(page_number,1),total_pages);
            const auto begin=(effective-1)*2, end=std::min(effective*2,all.size());
            require(p.total==all.size() && p.page_count==total_pages && p.effective_page==effective
                && p.rows.size()==end-begin,"rank clamping");
            for (std::size_t j=begin;j<end;++j) require(same_row(p.rows[j-begin],sorted[j]),"rank brute-force reference");
        }
    }
    refuses([&]{r.blocks_for_time_range('V','C',0,10);},"explicit clock required");
    refuses([&]{r.trade_rank_page("pnl",false,1,0);},"zero rank size");
    auto source=std::make_shared<Source>(b.bytes); auto scattered=reader::Reader::open(source);
    source->requests.clear();
    require(scattered.trade_rank_page("pnl",true,1,5).rows.size()==all.size(),"scattered page population");
    for (const auto& descriptor:b.descriptors[1]) {
        const auto offset=fixture::get(descriptor,0,8);
        const auto reads=std::count(source->requests.begin(),source->requests.end(),
            std::make_pair(offset,std::size_t(32)));
        require(reads==(compact?2:1),"rank page key scan/selected-block read count");
    }
}

struct Point { reader::EquityWitness witness; std::optional<double> mtm,book; };
Point point(const reader::Record& row) {
    Point p;
    p.witness.ordinal=row.ordinal; p.witness.time_ms=row.values[0].as_i64();
    if (!row.values[1].null) {
        p.mtm=row.values[1].as_f64(); p.witness.valid_mask|=1; p.witness.mtm_bits=row.values[1].word;
        const double v=*p.mtm-(row.values[2].null ? 0.0 : row.values[2].as_f64());
        if (std::isfinite(v)) { p.book=v; p.witness.valid_mask|=2; p.witness.book_bits=word(v); }
    }
    return p;
}
void equal_witness(const reader::EquityWitness& a,const reader::EquityWitness& z) {
    require(a.ordinal==z.ordinal && a.time_ms==z.time_ms && a.mtm_bits==z.mtm_bits
        && a.book_bits==z.book_bits && a.valid_mask==z.valid_mask,"paired witness/masks");
}
void test_envelope(bool compact) {
    for (const unsigned cap : {1u,3u,16u}) {
        fixture::Writer w; w.caps[0]=cap; w.compact=compact;
        auto input=population(); input[5].values[2]=std::monostate{};
        const auto b=w.encode(input); auto r=open(b);
        const auto rows=all_records(r,'E');
        for (const auto& filter : std::vector<std::optional<std::array<std::int64_t,2>>>{
                std::nullopt,std::array<std::int64_t,2>{{100,601}},std::array<std::int64_t,2>{{100,101}},
                std::array<std::int64_t,2>{{250,650}},std::array<std::int64_t,2>{{100,100}}}) {
            std::vector<Point> points;
            std::uint64_t finite_begin=0;
            for (const auto& row:rows) if (!row.values[1].null) {
                if (filter && row.values[0].as_i64()<(*filter)[0]) ++finite_begin;
                if (!filter || (row.values[0].as_i64()>=(*filter)[0] && row.values[0].as_i64()<(*filter)[1]))
                    points.push_back(point(row));
            }
            for (const unsigned requested : {1u,2u,3u,5u,20u,1200u}) {
                const auto got=r.equity_summary(requested,filter);
                const auto count=std::min<std::size_t>(requested,points.size());
                require(got.population==points.size() && got.buckets.size()==count,"envelope population/buckets");
                for (std::size_t j=0;j<count;++j) {
                    const auto begin=j*points.size()/count,end=(j+1)*points.size()/count;
                    const auto& bucket=got.buckets[j];
                    require(bucket.begin==finite_begin+begin && bucket.end==finite_begin+end,
                            "sample-index floor edges");
                    std::vector<std::size_t> selected{begin,end-1};
                    for (const bool book : {false,true}) {
                        std::vector<std::size_t> finite;
                        for (auto k=begin;k<end;++k) if (book?points[k].book.has_value():points[k].mtm.has_value()) finite.push_back(k);
                        const auto& summary=book?bucket.book:bucket.mtm;
                        require((!summary.finite_count_known || summary.finite_count==finite.size())
                            && summary.flags==(finite.empty()?9u:15u),"finite summary count/known scope");
                        equal_witness(summary.witnesses[0],points[begin].witness);
                        equal_witness(summary.witnesses[3],points[end-1].witness);
                        if (!finite.empty()) {
                            auto sorted=finite;
                            std::stable_sort(sorted.begin(),sorted.end(),[&](auto a,auto z) {
                                return (book?*points[a].book:*points[a].mtm)<(book?*points[z].book:*points[z].mtm); });
                            const auto low=sorted.front();
                            const double max_value=book?*points[sorted.back()].book:*points[sorted.back()].mtm;
                            const auto high=*std::find_if(finite.begin(),finite.end(),[&](auto k) {
                                return (book?*points[k].book:*points[k].mtm)==max_value; });
                            equal_witness(summary.witnesses[1],points[low].witness);
                            equal_witness(summary.witnesses[2],points[high].witness);
                            selected.push_back(low); selected.push_back(high);
                        }
                    }
                    std::sort(selected.begin(),selected.end()); selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
                    require(bucket.points.size()==selected.size(),"ordinal union");
                    for (std::size_t k=0;k<selected.size();++k) equal_witness(bucket.points[k],points[selected[k]].witness);
                }
            }
        }
        // An edge can cut a block many times: each E payload column is fetched
        // once in this envelope call, not once per intersecting bucket.
        auto source=std::make_shared<Source>(b.bytes); auto once=reader::Reader::open(source);
        source->requests.clear(); (void)once.equity_summary(20);
        std::vector<std::uint64_t> payload_reads;
        for (const auto& request:source->requests) {
            for (const auto& desc:b.descriptors[0]) {
                const auto start=fixture::get(desc,0,8),stop=start+fixture::get(desc,8,4);
                if (request.first>=start+176 && request.first<stop) payload_reads.push_back(request.first);
            }
        }
        std::sort(payload_reads.begin(),payload_reads.end());
        require(std::adjacent_find(payload_reads.begin(),payload_reads.end())==payload_reads.end(),"redecoded bucket-cut block");
    }
}

void brute_envelope(const reader::EquityEnvelope& got, const std::vector<reader::Record>& raw,
        std::uint64_t requested, const reader::SinkFolds& folds, bool adjusted,
        std::optional<std::array<std::int64_t,2>> filter = std::nullopt) {
    std::vector<reader::Record> finite;
    for (const auto& row : raw) if (!row.values[1].null) finite.push_back(row);
    const auto final_raw = finite.empty() ? UINT64_MAX : finite.back().ordinal;
    std::vector<Point> points;
    std::uint64_t before_filter = 0;
    for (auto row : finite) {
        if (filter && row.values[0].as_i64() < (*filter)[0]) ++before_filter;
        if (filter && (row.values[0].as_i64() < (*filter)[0] || row.values[0].as_i64() >= (*filter)[1])) continue;
        if (adjusted && row.ordinal == final_raw && folds.open_at_end_count
            && std::abs(row.values[2].null ? 0.0 : row.values[2].as_f64()) <= 1e-9) {
            row.values[2].null=false; row.values[2].word=word(folds.open_at_end_pnl);
        }
        points.push_back(point(row));
    }
    const auto b = std::min<std::uint64_t>(requested,points.size());
    require(got.population == points.size() && got.buckets.size() == b,"brute finite figure population");
    for (std::size_t j = 0; j < b; ++j) {
        const auto first=j*points.size()/b, end=(j+1)*points.size()/b;
        const auto& actual=got.buckets[j];
        require(actual.begin==before_filter+first && actual.end==before_filter+end,"brute finite floor edges");
        std::vector<std::size_t> selected{first,end-1};
        for (unsigned series=0;series<2;++series) {
            std::vector<std::size_t> sorted;
            for (auto k=first;k<end;++k) if (series?points[k].book.has_value():points[k].mtm.has_value()) sorted.push_back(k);
            const auto value=[&](std::size_t k) { return series?*points[k].book:*points[k].mtm; };
            std::stable_sort(sorted.begin(),sorted.end(),[&](auto a,auto z) { return value(a)<value(z); });
            const auto& s=series?actual.book:actual.mtm;
            require((!s.finite_count_known || s.finite_count==sorted.size()) && s.flags==(sorted.empty()?9u:15u),
                    "brute series finite count/flags");
            equal_witness(s.witnesses[0],points[first].witness); equal_witness(s.witnesses[3],points[end-1].witness);
            if (!sorted.empty()) {
                const auto low=sorted.front();
                const auto high=*std::find_if(sorted.begin(),sorted.end(),[&](auto k) { return value(k)==value(sorted.back()); });
                equal_witness(s.witnesses[1],points[low].witness); equal_witness(s.witnesses[2],points[high].witness);
                selected.push_back(low); selected.push_back(high);
            }
        }
        std::sort(selected.begin(),selected.end()); selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
        require(actual.points.size()==selected.size(),"brute figure union count");
        for (std::size_t k=0;k<selected.size();++k) equal_witness(actual.points[k],points[selected[k]].witness);
    }
}
void equal_envelopes(const reader::EquityEnvelope& a,const reader::EquityEnvelope& z) {
    require(a.population==z.population && a.buckets.size()==z.buckets.size(),"profile figure populations");
    for (std::size_t j=0;j<a.buckets.size();++j) {
        const auto& x=a.buckets[j]; const auto& y=z.buckets[j];
        require(x.begin==y.begin && x.end==y.end && x.points.size()==y.points.size(),"profile bucket edges/union");
        for (std::size_t k=0;k<x.points.size();++k) equal_witness(x.points[k],y.points[k]);
        for (const bool book : {false,true}) {
            const auto& xs=book?x.book:x.mtm; const auto& ys=book?y.book:y.mtm;
            require(xs.flags==ys.flags && (!xs.finite_count_known || !ys.finite_count_known || xs.finite_count==ys.finite_count),
                    "profile summary selectors/counts");
            for (unsigned k=0;k<4;++k) equal_witness(xs.witnesses[k],ys.witnesses[k]);
        }
    }
}
void test_profiles() {
    std::vector<fixture::Row> input;
    for (std::int64_t n=0;n<1801;++n) {
        auto row=equity(100+n/2,100.0+static_cast<double>(n%31),n%3==0?0.0:2.0);
        if (n%11==0 || n==1800) row.values[1]=std::monostate{};
        if (n%13==0) row.values[2]=std::monostate{};
        input.push_back(std::move(row));
    }
    input[1799].values[1]=500.0; input[1799].values[2]=std::monostate{};
    for (std::int64_t n=1;n<=50;++n) {
        auto row=trade(n,static_cast<double>(n));
        row.values[10]=n%3==0?-6.0:n%3==1?6.0:-0.0;
        if (n%7==0) row.values[7]=std::monostate{};
        input.push_back(std::move(row));
    }
    fixture::Writer full; full.all=false; full.stored_digest=true; full.caps={{128,16,2,2,1,2,2,512}};
    auto compact=full; compact.compact=true;
    const auto fb=full.encode(input), cb=compact.encode(input);
    auto fs=std::make_shared<Source>(fb.bytes), cs=std::make_shared<Source>(cb.bytes);
    auto f=reader::Reader::open(fs), c=reader::Reader::open(cs);
    require(f.profile().value==reader::Profile::full && c.profile().value==reader::Profile::compact,"root profile selection");
    unverified_metadata(c);
    require(c.memory_stats().value.compact_descriptor_bytes<=12288 && c.memory_stats().value.live_index_pages==0,
            "compact whole-table cache bound");
    refuses([&]{(void)c.index_page(0,0);},"compact physical-page refusal");
    const auto raw=all_records(f,'E');
    std::vector<reader::Record> finite;
    for (const auto& row:raw) if (!row.values[1].null) finite.push_back(row);
    for (const auto& bounds : std::vector<std::array<std::uint64_t,2>>{
            {0,0},{0,4},{117,240},{1000,static_cast<std::uint64_t>(finite.size())},
            {static_cast<std::uint64_t>(finite.size()-1),static_cast<std::uint64_t>(finite.size())}}) {
        for (auto* r : {&f,&c}) for (const bool adjusted : {false,true}) {
            auto cursor=r->equity_slice(bounds[0],bounds[1],adjusted); reader::Record row;
            auto at=bounds[0];
            while (cursor.next(row)) {
                auto expected=finite[static_cast<std::size_t>(at++)];
                if (adjusted && at==finite.size()) {
                    expected.values[2].null=false; expected.values[2].word=word(r->sink_folds().value.open_at_end_pnl);
                }
                require(same_row(row,expected),"profile finite ordinal slice/optional final profit");
            }
            require(at==bounds[1],"finite ordinal slice endpoint");
        }
    }
    for (const auto key : {"n","exit","pnl","pct","ru","dd"}) for (const bool descending : {false,true}) {
        const auto a=f.trade_rank_page(key,descending,99,7), z=c.trade_rank_page(key,descending,99,7);
        require(a.total==z.total && a.page_count==z.page_count && a.effective_page==z.effective_page
            && a.from==z.from && a.to==z.to && a.rows.size()==z.rows.size(),"profile rank DTO");
        for (std::size_t j=0;j<a.rows.size();++j) require(same_row(a.rows[j],z.rows[j]),"profile rank rows");
    }
    for (const auto requested : {1u,7u,1200u}) for (const bool adjusted : {false,true}) {
        const auto a=f.equity_summary(requested,std::nullopt,adjusted), z=c.equity_summary(requested,std::nullopt,adjusted);
        equal_envelopes(a,z);
        brute_envelope(a,raw,requested,f.sink_folds().value,adjusted || requested==1200);
    }
    for (const auto& bounds : std::vector<std::array<std::int64_t,2>>{{100,102},{200,600},{800,1100}}) {
        const auto a=f.equity_summary(5,bounds,true), z=c.equity_summary(5,bounds,true);
        equal_envelopes(a,z); brute_envelope(a,raw,5,f.sink_folds().value,true,bounds);
    }
    fs->requests.clear(); (void)f.equity_summary(1200);
    for (const auto& request:fs->requests) for (const auto& d:fb.descriptors[0])
        require(request.first<fixture::get(d,0,8) || request.first>=fixture::get(d,0,8)+fixture::get(d,8,4),
                "FULL default figure decoded E");
    auto cold_source=std::make_shared<Source>(cb.bytes); auto cold=reader::Reader::open(cold_source);
    cold_source->requests.clear(); (void)cold.equity_summary(1200);
    for (const auto& d:cb.descriptors[0])
        require(std::count(cold_source->requests.begin(),cold_source->requests.end(),
            std::make_pair(fixture::get(d,0,8),std::size_t(32)))==1,"COMPACT default figure rescanned E");
    require(cold.memory_stats().value.peak_decoded_blocks==1,"COMPACT figure retained multiple decoded blocks");
    auto rank_cold=reader::Reader::open(std::make_shared<Source>(cb.bytes));
    (void)rank_cold.trade_rank_page("dd",true,5,3);
    require(rank_cold.memory_stats().value.peak_rank_scratch_bytes<=25*cb.rows[1].size()
        && rank_cold.memory_stats().value.peak_decoded_blocks==1,"COMPACT packed rank/buffer bound");
    unverified_metadata(c); // partial queries never authenticate the whole identity
    require(f.verify().scope==reader::VerificationScope::whole_file && c.verify().scope==reader::VerificationScope::whole_file,
            "both profile full verification");
    require(f.logical_sha256().value==c.logical_sha256().value && f.counts().value==c.counts().value
        && f.head().value.summary==c.head().value.summary,
            "profiles changed verified logical records/metadata");
    require(c.finite_equity_count().value==finite.size(),"verified finite E root count");
    for (const auto key : {"n","exit","pnl","pct","ru","dd"}) for (const bool descending : {false,true})
        require(f.trade_non_null_count(key,descending).value==c.trade_non_null_count(key,descending).value,"profile quantile population");
    std::string fv,cv;
    require(f.to_v1([&](auto bytes){fv+=bytes;return true;}).scope==reader::VerificationScope::whole_file
        && c.to_v1([&](auto bytes){cv+=bytes;return true;}).scope==reader::VerificationScope::whole_file && fv==cv,
        "profile v1 export equality");
    const auto trailer=fv.substr(fv.rfind("{\"trailer\""));
    const auto expected_counts="\"counts\":{\"E\":"+std::to_string(fb.rows[0].size())+",\"T\":"
        +std::to_string(fb.rows[1].size())+"}";
    require(trailer.find(expected_counts)!=std::string::npos,"v1 input/trailer count keys");
    require(trailer.find("\"I\"")==std::string::npos && trailer.find("\"O\"")==std::string::npos,"v1 trailer absent/I keys");
}

void replace(std::string& text,const std::string& from,const std::string& to) {
    const auto at=text.find(from); require(at!=std::string::npos,"test replacement missing");
    text.replace(at,from.size(),to);
}
fixture::Built worked_file(bool compact) {
    // Independent §13.1 construction, including the literal field ledger.
    // No reader/writer layout constants or production encoders are used.
    using namespace fixture;
    Built b;
    const std::string records=R"([{"type":"E","fields":["time_ms","equity","open_profit"],"types":["i64","f64?","f64?"],"units":["unix_ms","account_ccy","account_ccy"]},{"type":"T","fields":["n","side","entry_time","exit_time","entry_price","exit_price","qty","pnl","pnl_pct","max_runup","max_drawdown","commission","entry_bar_index","exit_bar_index","open_at_end","entry_incarnation","close_cause","entry_id","exit_id","exit_comment"],"types":["i64","str","i64","i64","f64?","f64?","f64?","f64?","f64?","f64?","f64?","f64?","i64","i64","bool","u64","i64","str","str","str"],"units":["","","unix_ms","unix_ms","quote_ccy","quote_ccy","qty","account_ccy","percent","account_ccy","account_ccy","account_ccy","script_bar","script_bar","","","","","",""]}])";
    const std::string rest=R"(,"run_id":"vec-equity4","engine":"1.7.0","records":)"+records;
    const std::string head=R"({"schema":"pf-report-sink","version":2)"+rest
        +R"(,"format":{"byte_order":"le","codec":"zstd","level":3,"block_records":{"E":4096,"T":1024,"I":65536},"block_bytes":1048576}})";
    b.v1_header=R"({"schema":"pf-report-sink","version":1)"+rest+"}\n";
    b.bytes=std::string("\x89PFS2\r\n\x1a",8)+little(head.size(),4)+head;
    b.bytes+=little(checksum(b.bytes),4);
    require(b.bytes.size()==947,"worked HEAD947 ledger");
    b.rows[0]={equity(1700000000000LL,10000.0,0.0),equity(1700000060000LL,10005.25,5.25),
        equity(1700000120000LL,10010.5,10.5),equity(1700000180000LL,10010.5,0x1p-20)};
    for (unsigned j=0;j<4;++j) b.rows[7].push_back({'I',{u('E')}});
    const auto schema=definitions(false);
    std::array<Hash,8> streams{};
    for (unsigned t=0;t<8;++t) {
        std::string bytes=std::string("PFS2-STREAM\0",12)+tags[t];
        if (t==0) for (const auto& row:b.rows[0]) bytes+=canonical_row(row,schema[0]);
        if (t==7) bytes+="EEEE";
        streams[t]=hash(bytes);
    }
    std::array<Hash,2> blocks{};
    const std::array<std::string,3> raw{{
        std::string("\x80\xa0\xab\xfe\xf9\x62\xe0\xd4\x03\x02\x02\x02",12),
        std::string("\x02\x80\x89\x7a\x8d\x04\x02\x02\x00",9),
        std::string(24,'\0')+std::string("\0\x15\x30\x95\0\x40\0\x7e",8)}};
    for (unsigned which=0;which<2;++which) {
        const char tag=which?'I':'E';
        std::string directory, shas, payload;
        const unsigned modes[]={1,3,2};
        for (unsigned c=0;c<(which?1u:3u);++c) {
            std::string values=std::string("PFS2-COL\0",9)+tag+little(c,4)+little(0,8)+little(4,4);
            if (which) values+="EEEE";
            else for (const auto& row:b.rows[0]) values+=canonical(row.values[c],schema[0].types[c]);
            const auto body=which?std::string("EEEE"):raw[c];
            directory+=little(0,1)+little(which?4:modes[c],1)+little(0,2)+little(body.size(),4)
                +little(body.size(),4)+little(checksum(values),4);
            shas+=raw_hash(hash(values)); payload+=body;
        }
        std::string h(32,'\0'); h.replace(0,4,"BLK2"); h[4]=tag;
        put(h,8,which?1:3,4); put(h,12,4,4); put(h,24,directory.size()+shas.size()+payload.size(),4);
        put(h,28,checksum(h.substr(0,28)+directory+shas),4);
        blocks[which]=hash(std::string("PFS2-BLOCK\0",11)+tag+little(0,8)+little(4,4)+shas);
        const auto bytes=h+directory+shas+payload;
        require(bytes.size()==(which?84:229),"worked block ledger");
        b.bytes+=bytes;
    }
    require(b.bytes.size()==1260,"worked data_end1260");
    b.summary=R"({"engine":"pineforge","input":{},"applied_inputs":{},"applied_overrides":{},"applied_runtime":{},"elapsed_seconds":0.0,"summary":{},"metrics":{},"diagnostics":{},"fingerprint":null,"sink_folds":{"closed_count":0,"open_at_end_count":0,"open_at_end_pnl":0.0}})";
    require(b.summary.size()==257,"worked SUMMARY JSON257");
    const auto frame=std::string("\x28\xb5\x2f\xfd\x60\x01\x00\x09\x08\x00",10)+b.summary;
    require(frame.size()==267,"worked decode-only frame C267");
    b.bytes+="SUM2"+little(267,4)+little(257,4)+little(checksum(b.summary),4)+little(0,4)
        +raw_hash(hash(b.summary))+frame;
    require(b.bytes.size()==1579,"worked SUMMARY319 ends1579");
    std::string descriptors(192,'\0');
    put(descriptors,0,947,8); put(descriptors,8,229,4); descriptors[12]='E'; descriptors[13]=1;
    put(descriptors,16,4,4); put(descriptors,20,229,4);
    put(descriptors,32,1700000000000ULL,8); put(descriptors,40,1700000180000ULL,8);
    descriptors.replace(64,32,raw_hash(blocks[0]));
    put(descriptors,96,1176,8); put(descriptors,104,84,4); descriptors[108]='I';
    put(descriptors,112,4,4); put(descriptors,116,84,4); descriptors.replace(160,32,raw_hash(blocks[1]));
    std::string root(compact?320:4096,'\0'); root.replace(0,4,"FTR2"); put(root,4,2,2); put(root,6,compact,2);
    put(root,8,1260,8);
    if (compact) {
        b.bytes+=descriptors;
        put(root,16,319,4); put(root,20,2,4);
        put(root,24,4,8); put(root,80,4,8); put(root,88,4,8);
        root.replace(96,32,raw_hash(hash(b.summary))); root.replace(128,32,raw_hash(hash(b.v1_header)));
        put(root,160,1579,8); put(root,168,192,8);
        root.replace(176,32,raw_hash(hash(std::string("PFS2-CINDEX\0",12)+descriptors)));
        put(root,208,1,4); put(root,236,1,4);
    } else {
        put(root,16,1260,8); put(root,24,319,4); put(root,28,2,4);
        put(root,32,4,8); put(root,88,4,8); put(root,416,4,8); put(root,424,1200,4);
        for (unsigned t=0;t<8;++t) root.replace(96+32*t,32,raw_hash(streams[t]));
        root.replace(352,32,raw_hash(hash(b.summary))); root.replace(384,32,raw_hash(hash(b.v1_header)));
        for (unsigned table=0;table<19;++table) put(root,512+64*table+12,table<8?512:table==18?256:4,4);
        // Expand the literal W0..W3/descriptor/page ledgers independently.
        const std::array<std::uint64_t,4> mtm{{UINT64_C(0x40c3880000000000),UINT64_C(0x40c38aa000000000),
            UINT64_C(0x40c38d4000000000),UINT64_C(0x40c38d4000000000)}};
        std::array<std::string,4> witnesses;
        for (unsigned j=0;j<4;++j) {
            const auto book=j==3?UINT64_C(0x40c38d3ffff80000):UINT64_C(0x40c3880000000000);
            witnesses[j]=little(j,8)+little(1700000000000ULL+60000*j,8)+little(mtm[j],8)+little(book,8)
                +little(3,4)+little(0,4);
        }
        auto e=descriptors.substr(0,96); e.resize(512,'\0'); e[13]=5;
        for (unsigned which=0;which<2;++which) {
            const auto at=96+192*which;
            put(e,at,4,4); put(e,at+4,15,4);
            const unsigned selected[]={0,0,which?3u:2u,3};
            for (unsigned k=0;k<4;++k) e.replace(at+8+40*k,40,witnesses[selected[k]]);
        }
        auto order=descriptors.substr(96,96); order.resize(512,'\0');
        std::string figure;
        for (const auto& witness:witnesses) figure+=std::string(1,'\1')+std::string(15,'\0')+witness+std::string(200,'\0');
        auto page=[&](unsigned table,unsigned width,unsigned count,const std::string& payload) {
            std::string bytes(4096,'\0'); bytes.replace(0,4,"IDX2"); bytes[4]=static_cast<char>(table);
            put(bytes,12,count,4); put(bytes,16,width,4); put(bytes,20,1,4); bytes+=payload;
            const auto leaf=hash(std::string("PFS2-INDEX\0",11)+bytes.substr(0,24)+payload);
            bytes.replace(24,32,raw_hash(leaf)); put(bytes,4092,checksum(bytes.substr(0,4092)+payload),4);
            const auto locator=512+64*table;
            put(root,locator,b.bytes.size(),8); put(root,locator+8,1,4); put(root,locator+16,count,8);
            put(root,locator+24,bytes.size(),4); put(root,locator+28,1,4); root.replace(locator+32,32,raw_hash(leaf));
            b.bytes+=bytes;
        };
        page(0,512,1,e); require(b.bytes.size()==6187,"worked FULL E page end6187");
        page(7,512,1,order); require(b.bytes.size()==10795,"worked FULL I page end10795");
        page(18,256,4,figure);
    }
    put(root,root.size()-4,checksum(root.substr(0,root.size()-4)),4);
    require(b.bytes.size()==(compact?1771u:15915u),"worked selected root offset");
    b.bytes+=root;
    require(b.bytes.size()==(compact?2091u:20011u),"worked selected tail offset");
    std::string tail(160,'\0'); put(tail,0,compact?1771:15915,8); put(tail,8,root.size(),4);
    put(tail,16,compact?2251:20171,8);
    tail.replace(24,32,raw_hash(hash(root)));
    std::string identity=std::string("PFS2-LOGICAL\0",13)+raw_hash(hash(b.v1_header))+raw_hash(hash(b.summary));
    for (unsigned t=0;t<8;++t) identity+=tags[t]+little(t==0||t==7?4:0,8)+raw_hash(streams[t]);
    tail.replace(56,32,raw_hash(hash(identity))); tail.replace(152,8,"PFS2END\n");
    b.bytes+=tail;
    require(b.bytes.size()==(compact?2251u:20171u),"worked FULL EOF20171 / COMPACT EOF2251");
    return b;
}
void repair_root(std::string& bytes) {
    const auto tail=bytes.size()-160, at=static_cast<std::size_t>(fixture::get(bytes,tail,8));
    const auto length=static_cast<std::size_t>(fixture::get(bytes,tail+8,4));
    fixture::put(bytes,at+length-4,fixture::checksum(bytes.substr(at,length-4)),4);
    bytes.replace(tail+24,32,fixture::raw_hash(fixture::hash(bytes.substr(at,length))));
}
void pad_summary(std::string& json, std::size_t length) {
    const std::string member="\"padding\":\"\",";
    require(json.size()+member.size()<=length,"fixture SUMMARY padding target");
    json.insert(1,"\"padding\":\""+std::string(length-json.size()-member.size(),'x')+"\",");
    require(json.size()==length,"fixture exact decoded SUMMARY length");
}
std::string padded_summary_frame(std::string_view json, bool single = true) {
    // Independently encode the supplied JSON with raw prefix/suffix and RLE
    // padding. The non-single-segment form declares a 1-MiB window even when
    // its total content is larger (the J-only refusal in sections 5.2/17.4).
    const std::string_view prefix="{\"padding\":\"";
    const auto end=json.find('"',prefix.size());
    require(json.size()>=65792 && json.substr(0,prefix.size())==prefix && end!=std::string_view::npos
        && std::all_of(json.begin()+static_cast<std::ptrdiff_t>(prefix.size()),
            json.begin()+static_cast<std::ptrdiff_t>(end),[](char c){return c=='x';}),
        "RLE SUMMARY padding fixture");
    std::string frame("\x28\xb5\x2f\xfd",4);
    frame+=fixture::little(single?0xa0:0x80,1);
    if (!single) frame+=fixture::little(0x50,1);
    frame+=fixture::little(json.size(),4)+fixture::little(prefix.size()<<3,3)+std::string(prefix);
    for (std::size_t at=prefix.size();at<end;) {
        const auto n=std::min<std::size_t>(131072,end-at);
        frame+=fixture::little((n<<3)|2,3)+'x'; // nonlast RLE block
        at+=n;
    }
    frame+=fixture::little(((json.size()-end)<<3)|1,3)+std::string(json.substr(end));
    return frame;
}
fixture::Writer summary_limit_writer(bool compact, unsigned fault) {
    fixture::Writer w; w.all=false; w.compact=compact;
    if (fault==0) {
        w.change_summary=[](auto& json){pad_summary(json,1048577);};
        w.summary_frame=[](std::string_view json){return padded_summary_frame(json,false);};
    } else if (fault==1) {
        // Nine raw blocks: J=1,048,574 plus 9+9*3 bytes makes C=1,048,610.
        // J/window, FCS, root range and decoded CRC/SHA are otherwise valid.
        w.change_summary=[](auto& json){pad_summary(json,1048574);};
        w.summary_frame=[](std::string_view json){return fixture::raw_frame(json,131071);};
    } else {
        require(fault==2,"fixture SUMMARY limit fault");
        w.summary_frame=[](std::string_view json) {
            const auto frame=fixture::raw_frame(json);
            require(static_cast<unsigned char>(frame[4])==0x60,"fixture two-byte FCS");
            return std::string("\x28\xb5\x2f\xfd\x80\x58",6)
                +fixture::little(json.size(),4)+frame.substr(7); // 2-MiB window only
        };
    }
    return w;
}
void check_summary_frame_info(const reader::Reader& r, const fixture::Built& b) {
    const auto result=r.summary_frame_info();
    const auto& info=result.value;
    const std::array<std::uint8_t,4> magic{{'S','U','M','2'}};
    const auto root=static_cast<std::size_t>(fixture::get(b.bytes,b.bytes.size()-160,8));
    const bool compact=fixture::get(b.bytes,root+6,2)==1;
    require(result.scope==r.verified().scope && info.magic==magic && info.flags==0
        && info.stored_len==fixture::get(b.bytes,root+(compact?16:24),4)-52
        && info.json_len==b.summary.size() && info.json_crc==fixture::checksum(b.summary)
        && info.json_sha256==fixture::hash(b.summary),"decoded/validated SUM2 header diagnostics");
}
void check_metadata_values(reader::Reader& r, const fixture::Built& b, reader::VerificationScope scope) {
    const auto tail=b.bytes.size()-160;
    const auto root=static_cast<std::size_t>(fixture::get(b.bytes,tail,8));
    const bool compact=fixture::get(b.bytes,root+6,2)==1;
    const auto counts=r.counts();
    require(counts.scope==scope, "record-count file state");
    for (std::size_t t=0;t<8;++t)
        require(counts.value[t]==fixture::get(b.bytes,root+(compact?24:32)+8*t,8), "returned root record count");
    const auto finite=r.finite_equity_count();
    const auto identity=r.logical_sha256();
    require(finite.scope==scope && finite.value==fixture::get(b.bytes,root+(compact?88:416),8),
            "returned finite count/file state");
    require(identity.scope==scope && fixture::raw_hash(identity.value)==b.bytes.substr(tail+56,32),
            "returned logical identity/file state");
    const auto profile=r.profile();
    require(profile.scope==scope && profile.value==(compact?reader::Profile::compact:reader::Profile::full),
            "returned root profile/file state");
    const auto tables=r.tables();
    require(tables.scope==scope, "table-locator file state");
    auto compact_offset=fixture::get(b.bytes,root+(compact?160:8),8);
    for (std::size_t t=0;t<8;++t) {
        const auto at=root+512+64*t;
        const auto& table=tables.value[t];
        if (compact) {
            const auto n=fixture::get(b.bytes,root+208+4*t,4);
            require(table.offset==compact_offset && table.entry_count==n && table.entry_width==96,
                    "returned COMPACT root locator/count");
            compact_offset+=96*n;
        } else {
            require(table.offset==fixture::get(b.bytes,at,8) && table.entry_count==fixture::get(b.bytes,at+16,8)
                && table.page_count==fixture::get(b.bytes,at+8,4) && table.flags==fixture::get(b.bytes,at+28,4)
                && table.last_page_bytes==fixture::get(b.bytes,at+24,4), "returned FULL root locator/count");
        }
    }
    const auto head=r.head();
    const auto summary=r.summary();
    require(head.scope==scope && head.value.v1_header==b.v1_header && head.value.summary==b.summary,
            "returned HEAD/SUMMARY/file state");
    std::string hex;
    for (const unsigned char c : b.bytes.substr(tail+56,32)) {
        hex+="0123456789abcdef"[c>>4]; hex+="0123456789abcdef"[c&15];
    }
    require(summary.scope==scope && summary.value.compare(0,b.summary.size()-1,b.summary,0,b.summary.size()-1)==0
        && summary.value.find("\"sha256\":\""+hex+"\"")!=std::string::npos
        && summary.value.find("\"bytes\":"+std::to_string(b.bytes.size()))!=std::string::npos,
        "returned SUMMARY tail values/file state");
    const auto all=r.trade_non_null_count("n"), pnl=r.trade_non_null_count("pnl");
    const auto non_null=std::count_if(b.rows[1].begin(),b.rows[1].end(),[](const auto& row){return !fixture::null(row.values[7]);});
    require(all.scope==scope && all.value==b.rows[1].size() && pnl.scope==scope
        && pnl.value==static_cast<std::uint64_t>(non_null), "returned per-key counts/file state");
    std::uint64_t closed=0, opened=0; double open_pnl=0.0;
    for (const auto& row:b.rows[1]) {
        if (std::get<bool>(row.values[14])) {
            ++opened;
            if (!fixture::null(row.values[7])) open_pnl+=std::get<double>(row.values[7]);
        } else ++closed;
    }
    const auto folds=r.sink_folds();
    require(folds.scope==scope && folds.value.closed_count==closed && folds.value.open_at_end_count==opened
        && word(folds.value.open_at_end_pnl)==word(open_pnl), "returned fold counts/values/file state");
    const auto status=r.verified();
    require(status.scope==scope && status.value==(scope==reader::VerificationScope::whole_file), "returned completion state");
    require(r.summary_frame_info().scope==scope, "SUM2 diagnostics lost file state");
    check_summary_frame_info(r,b);
}
void test_accessor_file_state() {
    const auto input=population();
    for (const bool compact : {false,true}) {
        fixture::Writer w; w.compact=compact;
        const auto b=w.encode(input);
        auto source=std::make_shared<Source>(b.bytes); auto r=reader::Reader::open(source);
        auto retained=r.iter_type('E'); reader::Record row;
        require(retained.next(row), "retained cursor first row");
        for (const auto scope : {reader::VerificationScope::unverified,reader::VerificationScope::whole_file}) {
            if (scope==reader::VerificationScope::whole_file) {
                require(r.verify([&](const auto& record) {
                    const auto t=fixture::tags.find(record.tag);
                    equal(record,b.rows[t][static_cast<std::size_t>(record.ordinal)]);
                    require(record.file_scope==reader::VerificationScope::unverified, "verify callback promoted before completion");
                    return true;
                }).scope==scope, "accessor test full verification");
            }
            check_metadata_values(r,b,scope);
            const auto generation=r.generation();
            const auto stats=r.memory_stats();
            require(generation.scope==scope && generation.value==source->generation(), "returned generation/file state");
            require(stats.scope==scope && stats.value.compact_descriptor_bytes==(compact?96*fixture::get(b.bytes,
                static_cast<std::size_t>(fixture::get(b.bytes,b.bytes.size()-160,8))+20,4):0),
                "returned diagnostics/file state");
            const auto entry=r.descriptor(0,0);
            require(entry.file_scope==scope && entry.n==fixture::get(b.descriptors[0][0],16,4)
                && entry.offset==fixture::get(b.descriptors[0][0],0,8), "returned descriptor count/offset/file state");
            {
                const std::vector<std::uint32_t> ids{0};
                const auto partial=r.read_columns(entry,ids);
                require(partial.file_scope==scope && partial.entry.file_scope==scope
                    && partial.scope==reader::VerificationScope::columns && partial.verified_columns==ids
                    && partial.columns[0].words[0]==fixture::numeric(b.rows[0][0].values[0]),
                    "partial-column integrity must not lift file state");
                const auto full=r.read_block(entry);
                require(full.file_scope==scope && full.scope==reader::VerificationScope::block
                    && full.row(0).file_scope==scope, "block integrity must not lift file state");
                equal(full.row(0),b.rows[0][0]);
            }
            auto typed=r.iter_type('E'), trades=r.trade_slice(1,b.rows[1].size()), finite=r.equity_slice(0,1);
            for (auto* cursor : {&typed,&trades,&finite}) {
                require(cursor->file_scope()==scope && cursor->next(row) && row.file_scope==scope, "cursor/row file state");
                equal(row,cursor==&trades?b.rows[1][0]:b.rows[0][0]);
            }
            auto empty=r.equity_slice(0,0);
            require(empty.file_scope()==scope && !empty.next(row), "empty cursor lost file state");
            auto merged=r.iter_merged();
            require(merged.file_scope()==scope && merged.next(row) && row.file_scope==scope, "merged cursor/row file state");
            equal(row,input[0]);
            const auto time=std::get<std::int64_t>(b.rows[0][0].values[0]);
            auto blocks=r.blocks_for_time_range('E','A',time,time+1); reader::BlockEntry selected;
            require(blocks.file_scope()==scope && blocks.next(selected) && selected.file_scope==scope
                && selected.n==entry.n && selected.offset==entry.offset, "time-query descriptor/file state");
            const auto rank=r.trade_rank_page("n",false,1,1);
            require(rank.scope==scope && rank.total==b.rows[1].size() && rank.rows.size()==1
                && rank.rows[0].file_scope==scope, "rank counts/rows/file state");
            equal(rank.rows[0],b.rows[1][0]);
            const auto raw=all_records(r,'E');
            const auto figure=r.equity_summary(1200);
            require(figure.scope==scope, "figure file state");
            brute_envelope(figure,raw,1200,r.sink_folds().value,true);
            if (!compact) {
                const auto page=r.index_page(0,0);
                require(page.scope==scope && page.value.used_entries==b.descriptors[0].size(),
                        "FULL index page count/file state");
            }
        }
        require(retained.file_scope()==reader::VerificationScope::whole_file && retained.next(row)
            && row.file_scope==reader::VerificationScope::whole_file, "cached cursor did not adopt completed file state");
        equal(row,b.rows[0][1]);
        require(r.verify([](const auto&){return false;}).scope==reader::VerificationScope::cancelled, "accessor test cancelled recheck");
        check_metadata_values(r,b,reader::VerificationScope::unverified);
        if (!compact) {
            // Consistent FULL metadata can still declare a false stream digest.
            // Open must expose that value provisionally; only records disprove it.
            auto forged=b;
            const auto tail=forged.bytes.size()-160;
            const auto root=static_cast<std::size_t>(fixture::get(forged.bytes,tail,8));
            forged.bytes[root+96]^=1;
            std::string logical=std::string("PFS2-LOGICAL\0",13)+forged.bytes.substr(root+384,32)
                +forged.bytes.substr(root+352,32);
            for (std::size_t t=0;t<8;++t)
                logical+=fixture::tags[t]+forged.bytes.substr(root+32+8*t,8)+forged.bytes.substr(root+96+32*t,32);
            forged.bytes.replace(tail+56,32,fixture::raw_hash(fixture::hash(logical)));
            repair_root(forged.bytes);
            auto provisional=open(forged);
            check_metadata_values(provisional,forged,reader::VerificationScope::unverified);
            refuses([&]{(void)provisional.verify();}, "FULL root stream digest is consistency-only at open", "decoded stream SHA");
            check_metadata_values(provisional,forged,reader::VerificationScope::unverified);
        }
    }
}
void test_summary_preallocation() {
    // A 64-KiB JSON forces both stored and decoded payloads above the probe
    // threshold. HEAD/root/table/header allocations are independently smaller.
    // Every hostile fixture and source is built BEFORE counting allocations.
    const std::array<const char*,14> faults{{
        "root cap","root lower bound","J zero","J cap",
        "C zero","C cap","root/C disagreement","window cap",
        "missing FCS","wrong FCS","dictionary","reserved FHD","SUM2 flags","frame magic"}};
    const std::array<const char*,14> diagnostics{{
        "summary length exceeds cap","summary length exceeds cap",
        "summary decoded length exceeds cap","summary decoded length exceeds cap",
        "summary stored length exceeds cap","summary stored length exceeds cap",
        "summary root/header length mismatch","window exceeds cap","no declared content size","content size differs",
        "dictionary/reserved","dictionary/reserved","summary magic/flags","zstd frame magic"}};
    for (const bool compact : {false,true}) {
        fixture::Writer base; base.all=false; base.compact=compact;
        base.change_summary=[](auto& j){pad_summary(j,65536);};
        for (unsigned fault=0;fault<faults.size();++fault) {
            auto w=base;
            if (fault<2) w.change_root=[compact,fault](auto& root) {
                fixture::put(root,compact?16:24,fault==0?1048662:52,4);
            };
            if (fault==3 || fault==5) w=summary_limit_writer(compact,fault==3?0:1);
            if (fault==2 || fault==4 || fault==6) w.change_summary_header=[fault](auto& header) {
                fixture::put(header,fault==2?8:4,fault==6?fixture::get(header,4,4)+1:0,4);
            };
            if (fault>=7 && fault<=11) w.summary_frame=[fault](std::string_view json) {
                auto frame=fixture::raw_frame(json);
                if (fault==7) return std::string("\x28\xb5\x2f\xfd\x80\x58",6)
                    +fixture::little(json.size(),4)+fixture::little((json.size()<<3)|1,3)+std::string(json);
                if (fault==8) return std::string("\x28\xb5\x2f\xfd\x00\x50",6)+frame.substr(7);
                if (fault==9) frame[5]^=1;
                if (fault==10) { frame[4]|=1; frame.insert(5,1,'\0'); }
                if (fault==11) frame[4]|=8;
                return frame;
            };
            if (fault==12) w.change_summary_header=[](auto& header){fixture::put(header,16,1,4);};
            if (fault==13) w.summary_frame=[](std::string_view json) {
                auto frame=fixture::raw_frame(json); frame[0]^=1; return frame;
            };
            auto b=w.encode({});
            auto source=std::make_shared<Source>(std::move(b.bytes));
            const std::string label=std::string("SUMMARY preallocation ")+faults[fault];
            allocation_probe::Counter allocations(65536);
            refuses([&]{(void)reader::Reader::open(source);},label,diagnostics[fault],
                fault==3||fault==5||fault==7
                    ?reader::Failure::summary_limit:reader::Failure::malformed);
            const auto large_allocations=allocations.finish();
            require(large_allocations==0,label+": allocated an unchecked stored/decoded payload");
            for (const auto& request:source->requests)
                require(request.second<65536,label+": fetched the frame body before admission");
        }
        if (reader::Reader::has_zstd()) {
            auto b=base.encode({});
            auto source=std::make_shared<Source>(std::move(b.bytes));
            allocation_probe::Counter allocations(65536);
            auto r=reader::Reader::open(source);
            const auto large_allocations=allocations.finish();
            require(large_allocations>=2,"SUMMARY allocation probe did not observe admitted frame/output");
            require(r.verify().scope==reader::VerificationScope::whole_file
                && r.head().value.summary==b.summary,"admitted 64-KiB SUMMARY exact decode");
        }
    }
}
void test_summary_tiny_refusals() {
    // Complete, single-fault files (17.4), with real CRC/SHA and root lengths.
    // Only the C-only case needs a full ~1-MiB frame; all others are <64 KiB.
    // The existing probe observes real C++ allocations during reader open.
    const std::array<const char*,5> faults{{"J above S_MAX","C above stored bound","window above S_MAX","missing FCS","wrong FCS"}};
    for (const bool compact : {false,true}) {
        for (unsigned fault=0;fault<faults.size();++fault) {
            fixture::Writer w; w.all=false; w.compact=compact;
            if (fault<3) w=summary_limit_writer(compact,fault);
            else w.summary_frame=[fault](std::string_view json) {
                auto frame=fixture::raw_frame(json);
                require(static_cast<unsigned char>(frame[4])==0x60,"tiny fixture two-byte FCS");
                if (fault==3) return std::string("\x28\xb5\x2f\xfd\x00\x50",6)+frame.substr(7);
                frame[5]^=1;
                return frame;
            };
            auto b=w.encode({});
            const auto root=static_cast<std::size_t>(fixture::get(b.bytes,b.bytes.size()-160,8));
            const auto sum=static_cast<std::size_t>(fixture::get(b.bytes,root+8,8));
            const auto c=fixture::get(b.bytes,sum+4,4), j=fixture::get(b.bytes,sum+8,4);
            require(fixture::get(b.bytes,root+(compact?16:24),4)==52+c && sum+52+c==root
                && j==b.summary.size() && fixture::get(b.bytes,sum+12,4)==fixture::checksum(b.summary)
                && b.bytes.substr(sum+20,32)==fixture::raw_hash(fixture::hash(b.summary)),
                "single-fault SUMMARY dependent lengths/decoded digests");
            if (fault<3) {
                const auto frame=sum+52;
                const bool single=(static_cast<unsigned char>(b.bytes[frame+4])&0x20)!=0;
                std::uint64_t window=j;
                if (!single) {
                    const auto wd=static_cast<unsigned char>(b.bytes[frame+5]);
                    const auto base=UINT64_C(1)<<(10+(wd>>3));
                    window=base+(base/8)*(wd&7);
                }
                require(fixture::get(b.bytes,frame+(single?5:6),4)==j,
                        "single-fault SUMMARY FCS must match J");
                require((j>1048576)==(fault==0) && (c>1048609)==(fault==1)
                    && (window>1048576)==(fault==2),"SUMMARY vector has more than one budget fault");
                require(fault!=0 || j==1048577,"J-only SUMMARY endpoint");
                require(fault!=1 || c==1048610,"C-only SUMMARY endpoint");
            }
            require(b.bytes.size()<(fault==1?1114112u:65536u),"hostile SUMMARY fixture size");
            auto source=std::make_shared<Source>(std::move(b.bytes));
            const std::string label=std::string("single-fault SUMMARY ")+faults[fault];
            allocation_probe::Counter allocations(65536);
            refuses([&]{(void)reader::Reader::open(source);},label,"",
                fault<3?reader::Failure::summary_limit:reader::Failure::malformed);
            const auto large_allocations=allocations.finish();
            require(large_allocations==0,label+": allocated from an unchecked length/window");
            require(!source->requests.empty(),label+": did not exercise range reader");
            const auto& last=source->requests.back();
            if (fault<2) {
                require(source->requests.size()==2 && last.first==sum && last.second==52,
                        label+": read beyond oversized SUM2 header");
                if (fault==0) require(source->size()<j,label+": file is not shorter than declared J");
            } else require(last.first==sum+52 && last.second==std::min<std::uint64_t>(c,18),
                           label+": fetched frame body before header admission");
            for (const auto& request:source->requests)
                require(request.second<65536,label+": fetched payload before admission");
            if (reader::Reader::has_zstd()) {
                // Repair this one fault, then decode and verify the complete
                // frame/file through the same reader, including its values.
                if (fault==0) w.change_summary=[](auto& json){pad_summary(json,1048576);};
                else if (fault==1) w.summary_frame=[](std::string_view json){return fixture::raw_frame(json);};
                else if (fault==2) w.summary_frame=[](std::string_view json) {
                    const auto frame=fixture::raw_frame(json);
                    return std::string("\x28\xb5\x2f\xfd\x80\x50",6)
                        +fixture::little(json.size(),4)+frame.substr(7);
                };
                else w.summary_frame=nullptr;
                const auto repaired=w.encode({});
                auto r=open(repaired);
                check_summary_frame_info(r,repaired);
                require(r.head().value.summary==repaired.summary
                    && r.verify().scope==reader::VerificationScope::whole_file,
                    label+": repaired single fault did not decode/verify");
            }
        }
        if (reader::Reader::has_zstd()) {
            fixture::Writer small; small.all=false; small.compact=compact;
            const auto b=small.encode({});
            auto r=open(b);
            const auto info=r.summary_frame_info();
            require(info.value.json_len==b.summary.size() && r.head().value.summary==b.summary,
                    "small SUMMARY no longer opens");
            check_summary_frame_info(r,b);
            require(r.verify().scope==reader::VerificationScope::whole_file,"small SUMMARY no longer verifies");
        }
    }
}
void test_summary_frames() {
    const std::array<const char*,21> faults{{
        "bare JSON","legacy 48-byte bare JSON","dictionary","missing FCS","wrong FCS","oversized window",
        "extra frame","trailing bytes","skippable frame","truncated raw block","reserved block type",
        "malformed compressed block","reserved FHD","truncated checksum","output above J","output below J",
        "SUM2 flags","decoded CRC","decoded SHA","root decoded SHA","SUM2 magic"}};
    for (const bool compact : {false,true}) {
        fixture::Writer base; base.all=false; base.compact=compact; base.stored_digest=true;
        const std::vector<fixture::Row> input{equity(100,1.0,0.0),equity(200,2.0,0.0)};
        for (unsigned fault=0;fault<faults.size();++fault) {
            auto w=base;
            if (fault<16) w.summary_frame=[fault](std::string_view json) {
                auto frame=fixture::raw_frame(json);
                require(static_cast<unsigned char>(frame[4])==0x60,"fixture two-byte FCS");
                switch (fault) {
                case 0: case 1: return std::string(json);
                case 2: frame[4]|=1; frame.insert(5,1,'\0'); break; // dictionary ID 0 still forbidden
                case 3: return std::string("\x28\xb5\x2f\xfd\x00\x50",6)+frame.substr(7);
                case 4: frame[5]^=1; break;
                case 5: return std::string("\x28\xb5\x2f\xfd\x80\x58",6)
                    +fixture::little(json.size(),4)+frame.substr(7);
                case 6: frame+=fixture::raw_frame("{}"); break;
                case 7: frame+='\0'; break;
                case 8: return std::string("\x50\x2a\x4d\x18",4)+fixture::little(json.size(),4)+std::string(json);
                case 9: frame.pop_back(); break;
                case 10: frame[7]|=6; break;
                case 11: return frame.substr(0,7)+fixture::little(13,3)+std::string(1,'\0');
                case 12: frame[4]|=8; break;
                case 13: frame[4]|=4; frame+=std::string(2,'\0'); break;
                case 14: return frame.substr(0,7)+fixture::little(json.size()<<3,3)+std::string(json)
                    +fixture::little(9,3)+"x"; // each block fits; total output exceeds J
                case 15: return frame.substr(0,7)+fixture::little(((json.size()-1)<<3)|1,3)
                    +std::string(json.substr(0,json.size()-1));
                }
                return frame;
            };
            if (fault==1) w.change_summary_header=[](auto& header){header.erase(4,4);};
            if (fault==16) w.change_summary_header=[](auto& header){fixture::put(header,16,1,4);};
            if (fault==17) w.change_summary_header=[](auto& header){header[12]^=1;};
            if (fault==18) w.change_summary_header=[](auto& header){header[20]^=1;};
            if (fault==19) w.change_root=[compact](auto& root){root[compact?96:352]^=1;};
            if (fault==20) w.change_summary_header=[](auto& header){header[0]^=1;};
            const auto b=w.encode(input);
            refuses([&]{(void)open(b);},std::string("SUMMARY refusal ")+faults[fault],
                fault==17?"summary CRC":fault==18||fault==19?"summary SHA":
                fault==14||fault==15?"SUMMARY":"",
                fault==5?reader::Failure::summary_limit:
                fault>=17&&fault<=19?reader::Failure::integrity:reader::Failure::malformed);
        }
        // Different raw-block boundaries in two legal frames of the same
        // length: SUMMARY header/root/index bytes stay identical, so only the
        // frame can explain a changed stored-prefix digest. No build assumed.
        auto first=base, split=base;
        first.summary_frame=[](std::string_view json){return fixture::raw_frame(json,(json.size()+1)/2);};
        split.summary_frame=[](std::string_view json){return fixture::raw_frame(json,(json.size()+1)/2+1);};
        const auto a=first.encode(input), b=split.encode(input);
        require(a.bytes!=b.bytes && a.bytes.size()==b.bytes.size() && a.summary==b.summary,
                "reframed SUMMARY fixture identity/length");
        const auto root=static_cast<std::size_t>(fixture::get(a.bytes,a.bytes.size()-160,8));
        const auto sum=static_cast<std::size_t>(fixture::get(a.bytes,root+8,8));
        const auto root_length=compact?320u:4096u;
        require(a.bytes.substr(root,root_length)==b.bytes.substr(root,root_length)
            && a.bytes.substr(sum,52)==b.bytes.substr(sum,52),"reframing changed decoded identity/root/header");
        require(a.bytes.substr(a.bytes.size()-160+88,32)!=b.bytes.substr(b.bytes.size()-160+88,32),
                "stored-prefix digest omitted physical SUMMARY frame");
        auto ar=open(a), br=open(b);
        require(ar.verify().scope==reader::VerificationScope::whole_file
            && br.verify().scope==reader::VerificationScope::whole_file
            && ar.logical_sha256().value==br.logical_sha256().value && ar.head().value.summary==a.summary
            && br.head().value.summary==a.summary,"logical identity depends on zstd frame");
        // Sections 3.7/9.1: both forms at J=S_MAX are admitted. The eight-block
        // raw fallback is exactly C=1,048,609, 52+C=1,048,661.
        for (const bool stored_cap : {false,true}) {
            auto maximum=base;
            maximum.change_summary=[](auto& json){pad_summary(json,1048576);};
            if (!stored_cap) maximum.summary_frame=[](std::string_view json) {
                return padded_summary_frame(json);
            };
            const auto mb=maximum.encode({});
            const auto tail=mb.bytes.size()-160;
            const auto root=static_cast<std::size_t>(fixture::get(mb.bytes,tail,8));
            const auto sum=static_cast<std::size_t>(fixture::get(mb.bytes,root+8,8));
            require(fixture::get(mb.bytes,sum+8,4)==mb.summary.size()
                && mb.summary.size()==1048576,"maximum decoded SUMMARY field ledger");
            const auto c=fixture::get(mb.bytes,sum+4,4);
            require(c<=1048609 && (!stored_cap || c==1048609),"maximum stored SUMMARY field ledger");
            require(fixture::get(mb.bytes,root+(compact?16:24),4)==52+c,"maximum SUMMARY root stored length");
            auto mr=open(mb);
            const auto info=mr.summary_frame_info();
            require(info.value.json_len==mb.summary.size() && info.value.stored_len==c
                && info.scope==reader::VerificationScope::unverified,"maximum returned SUMMARY lengths/state");
            check_summary_frame_info(mr,mb);
            require(mr.verify().scope==reader::VerificationScope::whole_file && mr.head().value.summary==mb.summary,
                    "maximum framed SUMMARY exact decode");
            check_summary_frame_info(mr,mb);
        }
    }
}
void test_compact_unverified_identity() {
    const auto b=worked_file(true);
    for (unsigned fault=0;fault<3;++fault) {
        auto bytes=b.bytes;
        if (fault==0) bytes[bytes.size()-160+56]^=1;
        if (fault==1) fixture::put(bytes,1771+88,3,8); // plausible but false finite count
        if (fault==2) {
            // Change raw E/I counts and descriptor n consistently, so open's
            // table totals still hold; the actual block headers remain n=4.
            fixture::put(bytes,1771+24,5,8); fixture::put(bytes,1771+80,5,8);
            fixture::put(bytes,1579+16,5,4); fixture::put(bytes,1579+96+16,5,4);
            bytes.replace(1771+176,32,fixture::raw_hash(fixture::hash(
                std::string("PFS2-CINDEX\0",12)+bytes.substr(1579,192))));
        }
        repair_root(bytes); // root CRC and tail root SHA are consistent
        auto tampered=b; tampered.bytes=bytes;
        auto r=reader::Reader::open(std::make_shared<Source>(std::move(bytes)));
        check_metadata_values(r,tampered,reader::VerificationScope::unverified);
        unverified_metadata(r);
        check_summary_frame_info(r,b);
        refuses([&]{(void)r.verify();},"tampered COMPACT identity/counts",fault==0?"logical SHA":"");
        check_metadata_values(r,tampered,reader::VerificationScope::unverified);
        unverified_metadata(r);
        check_summary_frame_info(r,b);
    }
    auto r=open(b);
    unverified_metadata(r);
    require(r.trade_rank_page("n",false,1,10).scope==reader::VerificationScope::unverified
        && r.equity_summary(1200).scope==reader::VerificationScope::unverified,
        "COMPACT query totals lost provisional scope");
    require(r.verify().scope==reader::VerificationScope::whole_file && r.verified().value,"COMPACT identity state promotion");
    require(r.counts().value[0]==4 && r.counts().value[7]==4 && r.finite_equity_count().value==4
        && r.head().value.summary==b.summary && r.summary().value.find("\"sha256\"")!=std::string::npos
        && r.tables().value[0].entry_count==1 && r.sink_folds().value.closed_count==0,"verified COMPACT metadata access");
    require(r.trade_rank_page("n",false,1,10).scope==reader::VerificationScope::whole_file
        && r.equity_summary(1200).scope==reader::VerificationScope::whole_file,
        "verified COMPACT query totals scope");
    require(fixture::raw_hash(r.logical_sha256().value)==b.bytes.substr(b.bytes.size()-160+56,32),
            "verified COMPACT tail identity");
    require(r.verify([](const auto&){return false;}).scope==reader::VerificationScope::cancelled,
            "cancelled COMPACT identity recheck");
    unverified_metadata(r);
    check_summary_frame_info(r,b);
    require(r.verify().scope==reader::VerificationScope::whole_file && r.verified().value,"COMPACT identity retry");
}
void test_worked_discovery() {
    const auto b=worked_file(true);
    for (const bool trusted : {false,true}) {
        auto source=std::make_shared<Source>(b.bytes);
        reader::ReaderOptions options; options.trusted_compact=trusted;
        auto r=reader::Reader::open(source,options);
        require(source->requests.front()==std::make_pair(trusted?UINT64_C(1771):UINT64_C(0),std::size_t(trusted?480:2251)),
                "compact tail-first suffix/complete short object");
        const auto discovery=trusted?2u:1u;
        require(source->requests.size()==discovery+5,"compact discovery plus bounded HEAD/SUMMARY request count");
        const auto root=static_cast<std::size_t>(fixture::get(b.bytes,b.bytes.size()-160,8));
        require(source->requests[1]==std::make_pair(fixture::get(b.bytes,root+8,8),std::size_t(52)),
                "compact SUM2 length admission precedes other range reads");
        if (trusted) require(source->requests[2]==std::make_pair(UINT64_C(1579),std::size_t(192)),"compact whole-table discovery");
        require(!r.verified().value && r.profile().value==reader::Profile::compact,"worked open scope/profile");
        const auto requests=source->requests.size();
        check_summary_frame_info(r,b);
        require(!r.verified().value && source->requests.size()==requests,"COMPACT SUM2 diagnostics read or verified streams");
        unverified_metadata(r);
        const auto rows=all_records(r,'E');
        for (std::size_t j=0;j<rows.size();++j) equal(rows[j],b.rows[0][j]);
        require(rows.size()==4 && r.verify().scope==reader::VerificationScope::whole_file,"worked example decode");
        require(r.head().value.summary==b.summary,"worked COMPACT decoded SUMMARY257");
        brute_envelope(r.equity_summary(1200),rows,1200,r.sink_folds().value,true);
        std::string v1;
        require(r.to_v1([&](auto bytes){v1+=bytes;return true;}).scope==reader::VerificationScope::whole_file
            && v1.find("\"counts\":{\"E\":4,\"T\":0}")!=std::string::npos,"v1 keeps present zero-count T key");
    }
    const auto full=worked_file(false);
    auto source=std::make_shared<Source>(full.bytes); auto r=reader::Reader::open(source);
    require(source->requests.front()==std::make_pair(static_cast<std::uint64_t>(full.bytes.size()-4256),std::size_t(4256)),
            "FULL one root+tail suffix");
    const auto requests=source->requests.size();
    check_summary_frame_info(r,full);
    require(!r.verified().value && source->requests.size()==requests,"FULL SUM2 diagnostics read or verified streams");
    require(r.head().value.summary==full.summary,"worked FULL decoded SUMMARY");
    source->requests.clear(); (void)r.descriptor(0,0);
    require(source->requests.size()==1 && source->requests.front().second==4096+512,"FULL one short descriptor-page discovery");
    require(r.verify().scope==reader::VerificationScope::whole_file,"worked FULL verification");
    const auto rows=all_records(r,'E');
    require(rows.size()==full.rows[0].size(),"worked FULL record population");
    for (std::size_t j=0;j<rows.size();++j) equal(rows[j],full.rows[0][j]);
    brute_envelope(r.equity_summary(1200),rows,1200,r.sink_folds().value,true);
    auto compact=open(b);
    require(compact.verify().scope==reader::VerificationScope::whole_file
        && compact.logical_sha256().value==r.logical_sha256().value,"worked profiles decoded logical equality");
    auto wrong=b.bytes; fixture::put(wrong,1771+6,0,2); repair_root(wrong);
    refuses([&]{(void)reader::Reader::open(std::make_shared<Source>(wrong));},"compact flag/full mismatch","profile");
    wrong=full.bytes;
    const auto root=static_cast<std::size_t>(fixture::get(wrong,wrong.size()-160,8));
    fixture::put(wrong,root+6,1,2); repair_root(wrong);
    refuses([&]{(void)reader::Reader::open(std::make_shared<Source>(wrong));},"full flag/compact mismatch","profile");
    wrong=b.bytes; wrong[1579]^=1;
    refuses([&]{(void)reader::Reader::open(std::make_shared<Source>(wrong));},"compact table SHA","table SHA");
    wrong=b.bytes; wrong[wrong.size()-160+56]^=1;
    auto provisional=reader::Reader::open(std::make_shared<Source>(wrong));
    unverified_metadata(provisional);
    refuses([&]{(void)provisional.verify();},"compact decoded logical digest","logical SHA");
    unverified_metadata(provisional);
    wrong=b.bytes; wrong.insert(1771,std::string(4096,'\0'));
    fixture::put(wrong,wrong.size()-160,1771+4096,8);
    fixture::put(wrong,wrong.size()-160+16,wrong.size(),8);
    refuses([&]{(void)reader::Reader::open(std::make_shared<Source>(wrong));},"COMPACT extra derived page","tiling");
    wrong=b.bytes; wrong[1579+13]|=4;
    wrong.replace(1771+176,32,fixture::raw_hash(fixture::hash(std::string("PFS2-CINDEX\0",12)+wrong.substr(1579,192))));
    repair_root(wrong);
    refuses([&]{(void)reader::Reader::open(std::make_shared<Source>(wrong));},"COMPACT descriptor summary flag","COMPACT descriptor");
}

void test_profile_bounds_and_presence() {
    fixture::Writer w; w.all=false; w.compact=true; w.soft_bytes=1048576;
    w.caps={{4096,1024,4096,4096,1,4096,8192,65536}};
    for (const auto n : {10979u,10980u}) {
        std::vector<fixture::Row> rows(n,equity(100,1.0,0.0));
        const auto b=w.encode(rows);
        if (n==10979) {
            auto r=open(b); require(r.verify().scope==reader::VerificationScope::whole_file,"COMPACT E inclusive ceiling");
        } else refuses([&]{(void)open(b);},"COMPACT E threshold","ceiling");
    }
    for (const auto n : {3313u,3314u}) {
        std::vector<fixture::Row> rows;
        for (unsigned j=1;j<=n;++j) rows.push_back(trade(j,static_cast<double>(j%7)));
        const auto b=w.encode(rows);
        if (n==3313) {
            auto r=open(b); require(r.verify().scope==reader::VerificationScope::whole_file,"COMPACT T inclusive ceiling");
            const auto page=r.trade_rank_page("pnl",true,999999,50);
            require(page.to==n && r.memory_stats().value.peak_rank_scratch_bytes<=25*n,"COMPACT maximum key scan bound");
        } else refuses([&]{(void)open(b);},"COMPACT T threshold","ceiling");
    }
    w.caps[0]=1; w.caps[7]=2;
    for (const auto n : {85u,86u}) {
        const auto b=w.encode(std::vector<fixture::Row>(n,equity(100,1.0,0.0)));
        if (n==85) {
            auto source=std::make_shared<Source>(b.bytes); auto r=reader::Reader::open(source);
            const auto root=static_cast<std::size_t>(fixture::get(b.bytes,b.bytes.size()-160,8));
            const auto table=fixture::get(b.bytes,root+160,8);
            require(source->requests.size()==7
                && source->requests[0]==std::make_pair(static_cast<std::uint64_t>(b.bytes.size()-4256),std::size_t(4256))
                && source->requests[1]==std::make_pair(fixture::get(b.bytes,root+8,8),std::size_t(52))
                && source->requests[2]==std::make_pair(table,std::size_t(12288-(4256-480))),
                "compact 128-block table reuses suffix after SUM2 length admission");
            require(r.memory_stats().value.compact_descriptor_bytes==96*128
                && r.verify().scope==reader::VerificationScope::whole_file,"COMPACT D128 boundary");
        } else refuses([&]{(void)open(b);},"COMPACT D129 threshold","ceiling");
    }
    const auto empty=w.encode({});
    auto er=open(empty);
    require(er.verify().scope==reader::VerificationScope::whole_file && er.memory_stats().value.compact_descriptor_bytes==0,
            "empty compact index domain hash");
    const auto page=er.trade_rank_page("pnl",true,99,50);
    require(page.total==0 && page.page_count==1 && page.from==0 && page.to==0,"empty COMPACT DTO");
    auto empty_bad=empty.bytes;
    const auto empty_root=static_cast<std::size_t>(fixture::get(empty_bad,empty_bad.size()-160,8));
    empty_bad[empty_root+176]^=1; repair_root(empty_bad);
    refuses([&]{(void)reader::Reader::open(std::make_shared<Source>(empty_bad));},"empty compact table digest","table SHA");
    for(const bool compact:{false,true}) {
        auto nulls=w;nulls.compact=compact;
        auto row=trade(1,0.0);
        for(unsigned c=4;c<=11;++c)row.values[c]=std::monostate{};
        auto r=open(nulls.encode({row}));
        require(r.verify().scope==reader::VerificationScope::whole_file,"all-null nullable T binary schema");
        for(const bool descending:{false,true}) {
            const auto p=r.trade_rank_page("pnl",descending,1,50);
            require(p.total==1 && p.rows.size()==1 && p.rows[0].values[7].null
                && r.trade_non_null_count("pnl",descending).value==0,"all-null quantile population/rank page");
        }
    }

    fixture::Writer full; full.all=false;
    const auto b=full.encode({equity(100,1.0,0.0),trade(1,1.0)});
    const auto root=static_cast<std::size_t>(fixture::get(b.bytes,b.bytes.size()-160,8));
    for (unsigned table=8;table<19;++table) for (const bool flag_only : {false,true}) {
        auto bytes=b.bytes; const auto at=root+512+64*table;
        if (flag_only) fixture::put(bytes,at+28,0,4);
        else {
            const auto width=fixture::get(bytes,at+12,4);
            bytes.replace(at,64,std::string(64,'\0')); fixture::put(bytes,at+12,width,4);
        }
        repair_root(bytes);
        refuses([&]{(void)reader::Reader::open(std::make_shared<Source>(bytes));},
                "required FULL rank/figure absence "+std::to_string(table));
    }
    {
        auto bytes=b.bytes; fixture::put(bytes,root+512+24,4607,4); repair_root(bytes);
        refuses([&]{(void)reader::Reader::open(std::make_shared<Source>(bytes));},"short final locator length","last_page_bytes");
    }
    {
        auto bytes=b.bytes; fixture::put(bytes,root+512+2*64+28,1,4); repair_root(bytes);
        refuses([&]{(void)reader::Reader::open(std::make_shared<Source>(bytes));},"empty FULL table PRESENT");
    }
    for(const unsigned fault:{0u,1u,2u}) {
        auto invalid=full;
        invalid.change_figure=[fault](auto& entry) {
            if(fault==0)entry[8]=6; // selector outside union
            if(fault==1)entry[16+16]^=1; // finite but false MTM witness, all wrappers valid
            if(fault==2)entry[1]=1; // reserved field
        };
        auto r=open(invalid.encode({equity(100,1.0,0.0),trade(1,1.0)}));
        refuses([&]{(void)r.verify();},"authenticated ID18 selector/witness/reserved refusal");
    }
    full.caps[0]=1;
    const auto many=full.encode(std::vector<fixture::Row>(121,equity(100,1.0,0.0)));
    auto source=std::make_shared<Source>(many.bytes); auto r=reader::Reader::open(source);
    source->short_at=r.tables().value[0].offset;
    refuses([&]{(void)r.descriptor(0,0);},"short NON-final page","short readRange");
    require(source->requests.back().second==65536,"non-final page request must be full");
    {
        auto bytes=many.bytes;
        const auto page_at=static_cast<std::size_t>(r.tables().value[0].offset);
        // Full transport length but a forged short used_entries cannot end a table.
        fixture::put(bytes,page_at+12,119,4);
        auto forged=reader::Reader::open(std::make_shared<Source>(bytes));
        refuses([&]{(void)forged.descriptor(0,0);},"short non-final page arithmetic","used_entries");
    }
}

void test_sha_summary_and_order() {
    for (const bool compact : {false,true}) {
        fixture::Writer w; w.all=false; w.compact=compact;
        const std::vector<fixture::Row> input{equity(100,1.0,0.0),equity(200,2.0,0.0)};
        auto bad=w;
        bad.change_column_shas=[](char tag,auto& bytes){if(tag=='E')bytes[32]^=1;};
        auto r=open(bad.encode(input));
        const auto e=r.descriptor(0,0);
        const auto good=r.read_columns(e,{0,2});
        require(good.verified_columns==std::vector<std::uint32_t>({0,2}) && good.scope==reader::VerificationScope::columns,
                "unrelated columns retain CRC+SHA scope");
        refuses([&]{(void)r.read_columns(e,{1});},"CRC-valid/SHA-invalid selected column","column SHA");
        refuses([&]{(void)r.read_block(e);},"CRC-valid/SHA-invalid full block","column SHA");
        const auto original=w.encode(input);
        auto bytes=original.bytes;
        const auto at=static_cast<std::size_t>(fixture::get(original.descriptors[0][0],0,8));
        bytes[at+32+3*16+32]^=1; // SHA table corrupted, bhead CRC deliberately repaired
        fixture::put(bytes,at+28,fixture::checksum(bytes.substr(at,28)+bytes.substr(at+32,3*48)),4);
        auto unauthenticated=reader::Reader::open(std::make_shared<Source>(bytes));
        refuses([&]{(void)unauthenticated.read_columns(unauthenticated.descriptor(0,0),{0});},
                "SHA table authentication before any column","block SHA");
        require(unauthenticated.memory_stats().value.peak_decoded_blocks==0,"unauthenticated SHA table allocated columns");
        for (const auto& mutation : std::vector<std::pair<std::string,std::string>>{
                {"\"input\":{},",""},{"\"input\":{}","\"input\":[]"},{"\"fingerprint\":{}","\"fingerprint\":[]"},
                {"\"elapsed_seconds\":-0.0","\"elapsed_seconds\":1e400"},
                {"\"elapsed_seconds\":-0.0","\"elapsed_seconds\":NaN"},
                {"\"metrics\":{}","\"metrics\":{\"n\":9007199254740992}"},
                {"\"metrics\":{}","\"metrics\":{\"duplicate\":1,\"duplicate\":2}"},
                {"\"summary\":{}","\"summary\":{\"bad\":\"\\ud800\"}"},
                {"\"closed_count\":0,\"open_at_end_count\":0","\"open_at_end_count\":0,\"closed_count\":0"},
                {"\"closed_count\":0","\"closed_count\":1"},
                {"\"open_at_end_pnl\":0.0","\"open_at_end_pnl\":\"0\""},
                {"\"diagnostics\":{}","\"diagnostics\":{},\"trades\":[]"},
                {"\"diagnostics\":{}","\"diagnostics\":{},\"report_sink\":{}"},
                {"\"diagnostics\":{}","\"diagnostics\":{},\"report_window\":[]"},
                {"\"diagnostics\":{}","\"diagnostics\":{},\"outputs\":{}"}}) {
            auto invalid=w; invalid.change_summary=[mutation](auto& j){replace(j,mutation.first,mutation.second);};
            refuses([&]{(void)open(invalid.encode(input));},"SUMMARY grammar "+mutation.first);
        }
        for (const unsigned fault : {0u,1u,2u,3u}) {
            auto invalid=w;
            invalid.change_summary=[fault](auto& j) {
                if(fault==0) j+=' ';
                if(fault==1) j.insert(1," ");
                if(fault==2) j.insert(1,std::string("\"bad\":\"")+static_cast<char>(0xff)+"\",");
                if(fault==3) j.insert(1,"\"deep\":"+std::string(64,'[')+"0"+std::string(64,']')+",");
            };
            refuses([&]{(void)open(invalid.encode(input));},"SUMMARY whitespace/UTF8/depth");
        }
        auto additive=w;
        additive.change_summary=[](auto& j) {
            j.insert(1,"\"extension\":{\"integer\":9007199254740991,\"float\":9007199254740992.0,\"small\":-1e-999,\"str\":\"\\n\"},");
            j.insert(1,"\"depth64\":"+std::string(63,'[')+"0"+std::string(63,']')+",");
            replace(j,"\"fingerprint\":{}","\"fingerprint\":null");
        };
        const auto kept=additive.encode(input); auto ar=open(kept);
        require(ar.verify().scope==reader::VerificationScope::whole_file && ar.head().value.summary==kept.summary,
                "SUMMARY additive byte retention/safe numeric/valid depth");
        auto fold=w; fold.change_summary=[](auto& j){replace(j,"\"open_at_end_pnl\":0.0","\"open_at_end_pnl\":1.0");};
        auto fr=open(fold.encode(input));
        refuses([&]{(void)fr.verify();},"SUMMARY canonical T fold replay","sink_folds");

        fixture::Writer reordered; reordered.compact=compact; reordered.regroup_blocks=true;
        const auto p=population();
        const auto repacked=reordered.encode(p); auto rr=open(repacked);
        require(rr.verify().scope==reader::VerificationScope::whole_file,"reader must accept tiling-only cross-tag order");
        auto cursor=rr.iter_merged(); reader::Record row; std::size_t j=0;
        while(cursor.next(row)) {require(j<p.size(),"regrouped extra I record");equal(row,p[j++]);}
        require(j==p.size(),"regrouped I call-order reconstruction");
        auto canonical=w; canonical.run_id=std::string("q\"\\\n\0",5)+u8"雪🌲";
        auto cr=open(canonical.encode(input));
        require(cr.verify().scope==reader::VerificationScope::whole_file && cr.head().value.run_id==canonical.run_id,
                "canonical HEAD v1 escaping/UTF8/NUL round trip");
        auto escaping=w;
        escaping.change_head=[](auto& j){replace(j,"reader-test","reader\\u002dtest");};
        refuses([&]{(void)open(escaping.encode(input));},"HEAD alternate Unicode spelling","HEAD string escaping");
        escaping.run_id="a\nb"; escaping.change_head=[](auto& j){replace(j,"a\\u000ab","a\\nb");};
        refuses([&]{(void)open(escaping.encode(input));},"HEAD short control escape","HEAD string escaping");
        std::vector<fixture::Row> trades;
        for(std::int64_t n=1;n<=6;++n)trades.push_back(trade(n,0.0));
        trades[1].values[7]=1e16;trades[3].values[7]=-1e16;trades[5].values[7]=1.0;
        auto fold_reader=open(w.encode(trades));
        require(fold_reader.verify().scope==reader::VerificationScope::whole_file
            && word(fold_reader.sink_folds().value.open_at_end_pnl)==word(1.0),"canonical cancellation-sensitive T fold bits");
    }
    // A finite ordinal interval crosses an actual descriptor-page boundary,
    // including entirely null blocks and a long null-equity run.
    fixture::Writer w; w.all=false; w.caps[0]=1; w.caps[7]=16;
    std::vector<fixture::Row> input;
    for (std::int64_t j=0;j<260;++j) {
        auto row=equity(j,static_cast<double>(j),0.0);
        if(j%3==0 || (j>=118 && j<125)) row.values[1]=std::monostate{};
        input.push_back(std::move(row));
    }
    auto r=open(w.encode(input));
    const auto raw=all_records(r,'E');
    std::vector<reader::Record> finite;
    for(const auto& row:raw)if(!row.values[1].null)finite.push_back(row);
    const auto begin=static_cast<std::size_t>(std::count_if(input.begin(),input.begin()+118,
        [](const auto& row){return !fixture::null(row.values[1]);}));
    auto cursor=r.equity_slice(begin,begin+10); reader::Record row; auto j=begin;
    while(cursor.next(row))require(same_row(row,finite[j++]),"finite_prefix/null run across descriptor pages");
    require(j==begin+10 && r.verify().scope==reader::VerificationScope::whole_file,"finite page verified completion");
    auto invalid=w;
    invalid.change_descriptor=[](char tag,std::uint64_t raw,auto& d){if(tag=='E'&&raw==120)fixture::put(d,480,0,8);};
    auto bad=open(invalid.encode(input));
    refuses([&]{(void)bad.verify();},"finite_prefix continuity replay","prefix discontinuity");
}

void test_report_object() {
    // Literal expected bytes are from the input projection and Python's
    // documented scalar spelling, never harvested from this reader's output.
    const std::string summary=R"({"engine":"pineforge","input":{},"applied_inputs":{},"applied_overrides":{},"applied_runtime":{},"elapsed_seconds":-0.0,"summary":{"net_pnl":5.0,"avg_trade":5.0,"best_trade":5.0,"worst_trade":5.0,"max_drawdown":0.0},"diagnostics":{},"metrics":{},"fingerprint":{"name":")"
        +std::string(u8"雪🌲")+R"(","control":"\u0008\u000c\u000a\u000d\u0009\u007f"}})";
    const std::string before_trades=R"({"engine":"pineforge","input":{},"applied_inputs":{},"applied_overrides":{},"applied_runtime":{},"elapsed_seconds":-0.0,"summary":{"net_pnl":5.0,"avg_trade":5.0,"best_trade":5.0,"worst_trade":5.0,"max_drawdown":0.0},"diagnostics":{})";
    const std::string expected=before_trades
        +R"(,"trades":[{"n":1,"side":"long","entry_time":289,"exit_time":583,"entry_price":10.0,"exit_price":12.0,"qty":1.0,"pnl":5.0,"pnl_pct":0.5,"max_runup":-0.0,"max_drawdown":3.0,"commission":0.25,"entry_bar_index":0,"exit_bar_index":2,"open_at_end":false,"entry_incarnation":1}],"metrics":{},"equity_curve":[{"time_ms":100,"equity":-0.0,"open_profit":1e-07},{"time_ms":100,"equity":0.0,"open_profit":0.0}],"fingerprint":{"name":"\u96ea\ud83c\udf32","control":"\b\f\n\r\t\u007f"}})"
        +"\n";
    for (const bool compact : {false,true}) {
        fixture::Writer w; w.all=false; w.compact=compact; w.summary=summary;
        const std::vector<fixture::Row> input{equity(100,-0.0,1e-7),trade(1,5.0),
            equity(100,0.0,0.0)};
        auto r=open(w.encode(input));
        std::string actual;
        const auto result=r.to_report_json([&](std::string_view bytes){actual+=bytes;return true;});
        sink::Sha256 digest; digest.update(expected);
        require(result.scope==reader::VerificationScope::whole_file && actual==expected
            && result.bytes==expected.size() && result.sha256==digest.hex_digest(),"report-object raw projection/ASCII/float/LF/SHA");
        require(result.sha256!=r.content_sha256(),"report-object digest reused sink-v1 identity");
        auto bad=input; bad[1].values[4]=std::monostate{};
        auto invalid=open(w.encode(bad)); unsigned calls=0;
        try {
            (void)invalid.to_report_json([&](auto){++calls;return true;});
            throw std::runtime_error("null T report field accepted");
        } catch(const reader::ReaderError& e) {
            require(e.failure()==reader::Failure::report_result_invalid && calls==0
                && std::string(e.what()).find("field=trades.entry_price reason=nonfinite")!=std::string::npos,
                "typed report refusal before any success bytes");
        }
        auto null_equity=input;
        null_equity.back().values[1]=std::monostate{};
        auto invalid_equity=open(w.encode(null_equity)); calls=0;
        try {
            (void)invalid_equity.to_report_json([&](auto){++calls;return true;});
            throw std::runtime_error("null E report field accepted");
        } catch(const reader::ReaderError& e) {
            require(e.failure()==reader::Failure::report_result_invalid && calls==0
                && std::string(e.what()).find("field=equity_curve.equity reason=nonfinite")!=std::string::npos,
                "stage-A/v1 finite-E report contract");
        }
        auto cancellation=r.to_report_json([](auto){return false;});
        require(cancellation.scope==reader::VerificationScope::cancelled && cancellation.sha256.empty() && cancellation.bytes==0,
                "cancelled report-object receipt");
        const std::string manifest=R"({"series":[{"slot":1,"output":"second"},{"slot":0,"output":"color","encoding":"rgba-u32"},{"slot":1,"output":"repeat"}],"constants":[{"index":0,"encoding":"rgba-u32"},{"index":1}],"outputs":[{"index":1,"id":"old"},{"index":1,"id":"alarm","kind":"alert","freq":"once"},{"index":2,"id":"line","kind":"hline","price":{"constant":true}}]})";
        sink::Sha256 mh; mh.update(manifest);
        const auto common=R"({"schema_version":"pineforge-outputs/v1","message_format":"pineforge/v1","manifest_sha256":")"
            +mh.hex_digest()+R"(","manifest":)"+manifest;
        const auto saved=common+R"(,"constants":[4294967295,-1.25],"hlines":[{"output":"line","price":-1.25}]})";
        const auto expanded=common+R"(,"bars":{"open_ms":[null],"close_ms":[101]},"series":[{"slot":0,"output":"color","values":[1]},{"slot":1,"output":"second","values":[-0.0]},{"slot":1,"output":"repeat","values":[-0.0]}],"constants":[4294967295,-1.25],"hlines":[{"output":"line","price":-1.25}],"events":[{"sequence":0,"output":"alarm","bar_index":0,"bar_open_ms":null,"bar_close_ms":101,"ordinal_in_bar":0,"phase":"7","value":null,"message":"\u96ea\n","freq":"once"},{"sequence":1,"output":"line","bar_index":0,"bar_open_ms":100,"bar_close_ms":null,"ordinal_in_bar":1,"phase":"warmup","value":-0.0,"message":""}]})";
        auto enabled=w; enabled.all=true;
        replace(enabled.summary,"\"fingerprint\":","\"outputs\":"+saved+",\"fingerprint\":");
        auto with_outputs=input;
        with_outputs.push_back({'O',{i(0),std::monostate{},i(101),1.75,-0.0}});
        with_outputs.push_back({'K',{4294967295.75,-1.25}});
        with_outputs.push_back({'V',{u(0),i(1),i(0),std::monostate{},i(101),i(0),i(7),i(1),
            std::monostate{},u(UINT64_MAX),s(std::string(u8"雪")+"\n")}});
        with_outputs.push_back({'V',{u(1),i(2),i(0),i(100),std::monostate{},i(1),i(1),i(1),-0.0,u(0),s("")}});
        with_outputs.push_back({'X',{i(100),i(0),s("no report projection"),1.0}});
        with_outputs.push_back({'H',{u(UINT64_MAX)}});
        auto full_expected=expected;
        replace(full_expected,"\"fingerprint\":","\"outputs\":"+expanded+",\"fingerprint\":");
        auto output_reader=open(enabled.encode(with_outputs)); std::string expanded_actual;
        const auto output_result=output_reader.to_report_json([&](auto bytes){expanded_actual+=bytes;return true;});
        require(output_result.scope==reader::VerificationScope::whole_file && expanded_actual==full_expected,
                "report outputs clocks/stable series/rgba/K/hlines/event last-map/phase/Unicode");
        for (const auto& mutation : std::vector<std::pair<std::string,std::string>>{
                {"\"slot\":0","\"slot\":true"},{"\"slot\":0","\"slot\":2"},
                {"\"constants\":[4294967295,-1.25]","\"constants\":[0,-1.25]"},
                {"\"price\":-1.25","\"price\":0.0"}}) {
            auto bad_metadata=enabled;
            bad_metadata.change_summary=[mutation](auto& j){replace(j,mutation.first,mutation.second);};
            auto invalid_output=open(bad_metadata.encode(with_outputs)); unsigned emitted=0;
            refuses([&]{(void)invalid_output.to_report_json([&](auto){++emitted;return true;});},"report output metadata refusal");
            require(emitted==0,"invalid output metadata emitted report bytes before refusal");
        }
        fixture::Writer framing; framing.all=false; framing.compact=compact;
        auto synthetic=open(framing.encode({}));
        refuses([&]{(void)synthetic.to_report_json([](auto){return true;});},"synthetic SUMMARY cannot invent report");
    }
}

void test_refusals() {
    const std::vector<fixture::Row> small{equity(100,10.0,1.0),equity(200,11.0,2.0)};
    fixture::Writer ordinary; ordinary.all=false;
    const auto good=ordinary.encode(small);
    auto bad_bytes=[&](std::string bytes,const std::string& why,const std::string& diagnostic="") {
        refuses([&]{auto r=reader::Reader::open(std::make_shared<Source>(bytes)); (void)r.verify();},why,diagnostic);
    };
    for (const auto cut:std::vector<std::size_t>{0,1,159,good.bytes.size()-1,good.bytes.size()-160})
        bad_bytes(good.bytes.substr(0,cut),"truncated");
    bad_bytes(good.bytes+"x","bytes after tail");
    {auto bytes=good.bytes; bytes.back()^=1; bad_bytes(bytes,"bad tail magic","tail");}
    {auto bytes=good.bytes; bytes[0]^=1; bad_bytes(bytes,"bad head magic","magic");}
    {auto bytes=good.bytes; fixture::put(bytes,8,2097153,4);bad_bytes(bytes,"head allocation cap","head length");}
    {auto bytes=good.bytes; bytes[12+fixture::get(bytes,8,4)]^=1; bad_bytes(bytes,"head CRC","head CRC");}
    {auto bytes=good.bytes; const auto offset=fixture::get(good.descriptors[0][0],0,8);
     bytes[static_cast<std::size_t>(offset)+28]^=1; bad_bytes(bytes,"block CRC","directory CRC");}
    {auto bytes=good.bytes; const auto offset=fixture::get(good.descriptors[0][0],0,8);
     bytes[static_cast<std::size_t>(offset)+176]^=1; bad_bytes(bytes,"flipped stored column byte","column CRC");}
    {auto bytes=good.bytes; const auto root=fixture::get(bytes,bytes.size()-160,8);
     const auto sum=fixture::get(bytes,static_cast<std::size_t>(root)+16,8);
     bytes[static_cast<std::size_t>(sum)+12]^=1; bad_bytes(bytes,"summary CRC","summary CRC");}
    {auto bytes=good.bytes; const auto root=fixture::get(bytes,bytes.size()-160,8);
     const auto sum=fixture::get(bytes,static_cast<std::size_t>(root)+16,8);
     bytes[static_cast<std::size_t>(sum)+20]^=1; bad_bytes(bytes,"summary SHA","summary SHA");}
    {auto bytes=good.bytes; const auto at=static_cast<std::size_t>(fixture::get(bytes,bytes.size()-160,8));
     bytes[at+4092]^=1;
     bytes.replace(bytes.size()-160+24,32,fixture::raw_hash(fixture::hash(bytes.substr(at,4096))));
     bad_bytes(bytes,"root CRC","root CRC");}
    {auto bytes=good.bytes; bytes[bytes.size()-160+24]^=1; bad_bytes(bytes,"root SHA","root SHA");}
    {auto bytes=good.bytes; bytes[bytes.size()-160+56]^=1; bad_bytes(bytes,"tail logical SHA","logical SHA");}
    {auto bytes=good.bytes; const auto at=static_cast<std::size_t>(fixture::get(bytes,bytes.size()-160,8));
     const auto page=static_cast<std::size_t>(fixture::get(bytes,at+512,8)); bytes[page+4092]^=1;
     bad_bytes(bytes,"page CRC","page CRC");}
    auto mutation=[&](fixture::Writer w,const std::string& why,const std::string& diagnostic="") {
        bad_bytes(w.encode(small).bytes,why,diagnostic);
    };
    {
        auto w=ordinary; w.change_head=[](auto& j){replace(j,"\"version\":2","\"version\":3");};
        mutation(w,"unknown version");
    }
    {
        auto w=ordinary; w.change_head=[](auto& j){replace(j,"\"codec\":\"zstd\"","\"codec\":\"gzip\"");};
        mutation(w,"unknown format codec");
    }
    {
        auto w=ordinary; w.change_head=[](auto& j){replace(j,"\"time_ms\"","\"time_bad\"");};
        mutation(w,"fixed schema mismatch");
    }
    {
        auto w=ordinary; w.change_head=[](auto& j){replace(j,"\"run_id\":\"reader-test\"","\"run_id\":\"reader-test\",\"run_id\":\"twice\"");};
        mutation(w,"duplicate head key");
    }
    {
        auto w=ordinary; w.change_head=[](auto& j){j.insert(1," ");}; mutation(w,"noncompact head");
    }
    {
        auto w=ordinary; w.change_head=[](auto& j){replace(j,"reader-test","\\ud800");}; mutation(w,"surrogate string");
    }
    {
        auto w=ordinary; w.change_summary=[](auto& j){j.insert(1,"\"engine\":0,");}; mutation(w,"duplicate metadata key");
    }
    {
        auto w=ordinary; w.change_summary=[](auto& j){j="{\"engine\":"+std::string(65,'[')+std::string(65,']')+"}";};
        mutation(w,"summary depth");
    }
    {
        auto w=ordinary;
        w.change_summary=[](auto& json){pad_summary(json,1048577);};
        w.summary_frame=[](std::string_view json){return padded_summary_frame(json,false);};
        mutation(w,"summary cap","summary decoded length exceeds cap");
    }
    for (const auto root_case:std::vector<std::pair<std::size_t,std::uint64_t>>{
            {4,3},{6,1},{8,UINT64_MAX},{16,0},{24,0},{28,0},{32,UINT64_MAX},{88,0},
            {512,UINT64_MAX},{512+12,8},{512+16,UINT64_MAX},{512+24,1},{416,1},{1664,1}}) {
        auto w=ordinary; w.change_root=[root_case](auto& root){
            const unsigned width=root_case.first==4||root_case.first==6?2:
                root_case.first==24||root_case.first==28||root_case.first==524?4:8;
            fixture::put(root,root_case.first,root_case.second,width);
        };
        mutation(w,"root field/offset/count/padding "+std::to_string(root_case.first));
    }
    {
        auto w=ordinary; w.change_root=[](auto& root){root[96]^=1;}; mutation(w,"stream digest","stream SHA");
    }
    {
        auto w=ordinary;w.change_root=[](auto& root){root[384]^=1;};mutation(w,"v1 header digest","v1 header SHA");
    }
    for (const auto at:std::vector<std::size_t>{12,120}) {
        auto w=ordinary; w.change_tail=[at](auto& tail){tail[at]=static_cast<char>(0x80);}; mutation(w,"tail flags/padding");
    }
    {
        auto w=ordinary; w.stored_digest=true; w.change_tail=[](auto& tail){tail[88]^=1;};
        mutation(w,"stored prefix digest","stored prefix SHA");
    }
    for (const auto at:std::vector<std::size_t>{0,1,2,3,4}) {
        auto w=ordinary;
        w.change_directory=[at](char tag,auto& dir){
            if (tag!='E') return;
            if (at==0) dir[0]=2;                     // unknown codec
            if (at==1) dir[1]=7;                     // unknown/illegal mode
            if (at==2) dir[2]=1;                     // bitmap on required time
            if (at==3) dir[3]=1;                     // reserved byte
            if (at==4) fixture::put(dir,4,16777217,4);// unchecked allocation length
        };
        mutation(w,"directory refusal "+std::to_string(at));
    }
    {
        auto w=ordinary; w.change_directory=[](char tag,auto& dir){if(tag=='E') dir[12]^=1;};
        mutation(w,"decoded CRC","column CRC");
    }
    for (const auto at:std::vector<std::size_t>{5,6,8,12,16,24}) {
        auto w=ordinary; w.change_bhead=[at](char tag,auto& h){if(tag=='E') h[at]^=1;};
        // ReaderState::block checks flags/reserved/ncols/n/rec0/body in one
        // predicate, before CRC or SHA validation, for all six offsets.
        mutation(w,"block header mismatch "+std::to_string(at),"block/header descriptor mismatch");
    }
    {
        auto w=ordinary; w.change_descriptor=[](char tag,std::uint64_t,auto& d){if(tag=='E') d[64]^=1;};
        mutation(w,"decoded block SHA","block SHA");
    }
    {
        auto w=ordinary;w.change_descriptor=[](char tag,std::uint64_t,auto& d){if(tag=='E')fixture::put(d,8,33554433,4);};
        mutation(w,"stored block cap");
    }
    for (const auto at:std::vector<std::size_t>{13,14,480}) {
        auto w=ordinary; w.change_descriptor=[at](char tag,std::uint64_t,auto& d){if(tag=='E') d[at]|=static_cast<char>(0x80);};
        mutation(w,"descriptor flags/reserved "+std::to_string(at));
    }
    {
        auto w=ordinary; w.change_descriptor=[](char tag,std::uint64_t,auto& d){if(tag=='E') fixture::put(d,40,201,8);};
        mutation(w,"time range","time-range mismatch");
    }
    {
        auto w=ordinary; w.change_descriptor=[](char tag,std::uint64_t,auto& d){if(tag=='E') d[96+8+16]^=1;};
        mutation(w,"E summaries","witness summary mismatch");
    }
    {
        auto w=ordinary; w.change_column=[](char tag,std::size_t c,auto& e){
            if(tag=='E'&&c==1){e.flags=1;e.bytes.insert(0,1,static_cast<char>(0x80));}};
        mutation(w,"unused null bitmap bits","null bits");
    }
    {
        auto w=ordinary; w.change_column=[](char tag,std::size_t c,auto& e){
            if(tag=='E'&&c==1){e.bytes.assign(16,'\0');e.bytes[12]=static_cast<char>(0xf0);e.bytes[14]=0x7f;}};
        mutation(w,"nonfinite f64","non-finite");
    }
    for (const auto& body:std::vector<std::string>{
            std::string("\x80\0\1",3),std::string(9,static_cast<char>(0x80)),
            fixture::uv(UINT64_C(9007199254740992)),fixture::uv(200)+fixture::uv(0)+fixture::uv(2),
            fixture::uv(0)+fixture::uv(UINT64_C(9007199254740991))+fixture::uv(UINT64_C(4503599627370496)),
            fixture::uv(0)+fixture::uv(1)+fixture::uv(UINT64_C(9007199254740991))}) {
        auto w=ordinary; w.change_column=[body](char tag,std::size_t c,auto& e){if(tag=='E'&&c==0){e.mode=1;e.bytes=body;}};
        mutation(w,"malformed/nonminimal/overflow DELTA");
    }
    for (const auto& body:std::vector<std::string>{
            std::string(1,'\15')+fixture::uv(2)+fixture::uv(1)+fixture::uv(0),
            std::string(1,'\0')+fixture::uv(UINT64_C(2251799813685248))+fixture::uv(1)+fixture::uv(0)}) {
        auto w=ordinary; w.change_column=[body](char tag,std::size_t c,auto& e){if(tag=='E'&&c==1){e.mode=3;e.bytes=body;}};
        mutation(w,"DECIMAL scale/domain");
    }
    auto framed=[](std::string raw) {
        return std::string("\x28\xb5\x2f\xfd\x20",5)+fixture::little(raw.size(),1)
            +fixture::little((raw.size()<<3)|1,3)+raw;
    };
    for (unsigned fault=0;fault<5;++fault) {
        auto w=ordinary; w.change_column=[fault,framed](char tag,std::size_t c,auto& e){
            if(tag!='E'||c!=0) return;
            auto frame=framed(e.bytes);
            if(fault==0) frame=std::string("\x28\xb5\x2f\xfd\x80\x78",6)+fixture::little(e.bytes.size(),4);
            if(fault==1) frame=std::string("\x28\xb5\x2f\xfd\0\0",6);
            if(fault==2) frame[4]=0x21;
            if(fault==3) frame[5]^=1;
            if(fault==4) frame+='\0';
            e.codec=1;e.bytes=frame;
        };
        mutation(w,"zstd framing "+std::to_string(fault),fault==0?"window":"zstd");
    }
    fixture::Writer typed;
    auto all=population();
    auto typed_mutation=[&](fixture::Writer w,const std::string& label) {
        bad_bytes(w.encode(all).bytes,label);
    };
    {
        auto w=typed; w.change_column=[](char tag,std::size_t c,auto& e){
            if(tag=='T'&&c==14) e.bytes[0]=2;}; typed_mutation(w,"bool");
    }
    for(const auto c:std::vector<std::size_t>{15,16}) {
        auto w=typed; w.change_column=[c](char tag,std::size_t col,auto& e){
            if(tag=='T'&&col==c) e.bytes.replace(0,8,fixture::little(c==15?UINT64_MAX:UINT64_C(0x8000000000000000),8));};
        typed_mutation(w,"unsafe integer extreme");
    }
    {
        auto w=typed; w.change_column=[](char tag,std::size_t c,auto& e){
            if(tag=='T'&&c==17) e.bytes=std::string("\1\1\xc0\xaf",4);};
        typed_mutation(w,"invalid UTF-8");
    }
    {
        auto w=typed; w.change_column=[](char tag,std::size_t c,auto& e){
            if(tag=='T'&&c==17){e.mode=5;e.bytes=fixture::uv(1)+fixture::uv(1)+"a"+fixture::uv(1)+fixture::uv(0);}};
        typed_mutation(w,"dictionary index");
    }
    {
        auto w=typed; w.change_column=[](char tag,std::size_t c,auto& e){
            if(tag=='T'&&c==17) e.bytes+=std::string(1,'x');};
        typed_mutation(w,"trailing string body");
    }
    {
        auto bad=all; bad[1].values[1]=s("neither"); bad_bytes(typed.encode(bad).bytes,"side enum");
    }
    {
        auto w=typed; w.change_order=[](char tag){return tag=='T'?'E':tag;};
        typed_mutation(w,"I multiplicities");
    }
    {
        auto bad=small; bad[0].values[0]=i(200); bad[1].values[0]=i(100);
        bad_bytes(ordinary.encode(bad).bytes,"decreasing E");
    }
    {
        auto bad=all; bad[1].values[0]=i(2); bad_bytes(typed.encode(bad).bytes,"T.n order");
    }
    {
        auto bad=all; bad[2].values[0]=i(1); bad_bytes(typed.encode(bad).bytes,"O.row order");
    }
    for(unsigned fault=0;fault<3;++fault) {
        auto w=typed; w.change_rank=[fault](unsigned table,auto& positions){
            if(table!=11) return;
            if(fault==0) positions[1]=positions[0];
            if(fault==1) positions[0]=UINT32_MAX;
            if(fault==2) std::reverse(positions.begin(),positions.end());
        };
        typed_mutation(w,"rank permutation/comparator");
    }
    {
        auto w=ordinary; w.change_page=[](unsigned table,unsigned,auto& page){if(table==0)page[24]^=1;};
        mutation(w,"index leaf","leaf SHA");
    }
    for(const auto at:std::vector<std::size_t>{56,1080,4091}) {
        auto w=ordinary;w.change_page=[at](unsigned table,unsigned,auto& page){if(table==0)page[at]=1;};
        mutation(w,"index unused proof/padding "+std::to_string(at),"reserved/padding");
    }
    std::vector<fixture::Row> many;
    for(std::int64_t n=0;n<241;++n) many.push_back(equity(n,1.0,0.0));
    {
        auto w=ordinary; w.caps[0]=1;
        w.change_page=[](unsigned table,unsigned p,auto& page){if(table==0&&p==0)page[56]^=1;};
        bad_bytes(w.encode(many).bytes,"Merkle sibling","Merkle");
    }
    {
        auto w=ordinary; w.caps[0]=1; w.bad_merkle_padding=true;
        bad_bytes(w.encode(many).bytes,"Merkle repeated-last padding","Merkle");
    }
    {
        fixture::Writer w; w.slots=600; w.caps[2]=65536; w.caps[7]=65536;
        fixture::Row output{'O',{i(0),std::monostate{},std::monostate{}}};
        output.values.insert(output.values.end(),600,fixture::Cell(0.0));
        w.change_bhead=[](char tag,auto& h){if(tag=='O')fixture::put(h,12,65536,4);};
        w.change_descriptor=[](char tag,std::uint64_t,auto& d){if(tag=='O')fixture::put(d,16,65536,4);};
        w.change_root=[](auto& root){fixture::put(root,48,65536,8);fixture::put(root,88,65536,8);};
        const auto b=w.encode({output}); auto r=open(b);
        refuses([&]{(void)r.verify();},"decoded budget before allocation","U budget before allocation");
        require(r.memory_stats().value.peak_decoded_blocks==0,"oversized decoded budget allocated columns");
    }
}

struct TempPath {
    std::string path;
    TempPath() {
        auto pattern=(std::filesystem::temp_directory_path()/"pineforge-l21c-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(),pattern.end()); buffer.push_back('\0');
        const int fd=::mkstemp(buffer.data()); require(fd>=0,"temporary v1 sink");
        ::close(fd); path=buffer.data();
    }
    ~TempPath() { std::error_code error; std::filesystem::remove(path,error); }
};
void test_v1_conversion() {
    auto input=population();
    // The v1 writer's C structs have int32 bar/output indexes. The binary
    // schema admits safe i64; those extremes are covered separately above.
    for(auto& row:input) {
        // Current LineWriter refuses null E; nullable binary/query coverage
        // remains in population() and the independent schema tests above.
        if(row.tag=='E') for(unsigned c=1;c<=2;++c)
            if(fixture::null(row.values[c])) row.values[c]=0.0;
        if(row.tag=='X') row.values[1]=i(1);
        if(row.tag=='V') row.values[1]=i(1);
    }
    TempPath path;
    const pf_report_sink_v1 config{sizeof(pf_report_sink_v1),1,path.path.c_str(),"reader-test",nullptr};
    sink::Trailer trailer{};
    {
        sink::LineWriter w(config); w.write_header({true,true,true,2,2});
        for(const auto& row:input) {
            const auto& cells=row.values;
            auto signed_value=[&](std::size_t c)->std::int64_t {return fixture::null(cells[c])?INT64_MIN:std::get<std::int64_t>(cells[c]);};
            auto float_value=[&](std::size_t c) {return fixture::null(cells[c])?std::numeric_limits<double>::quiet_NaN():std::get<double>(cells[c]);};
            auto string_value=[&](std::size_t c)->const std::string& {return std::get<std::string>(cells[c]);};
            switch(row.tag) {
            case 'E': w.write_e({signed_value(0),float_value(1),float_value(2)}); break;
            case 'T': {
                pf_trade_t t{};
                t.is_long=string_value(1)=="long"; t.entry_time=signed_value(2); t.exit_time=signed_value(3);
                t.entry_price=float_value(4);t.exit_price=float_value(5);t.qty=float_value(6);
                t.pnl=float_value(7);t.pnl_pct=float_value(8);t.max_runup=float_value(9);t.max_drawdown=float_value(10);
                t.commission=float_value(11);t.entry_bar_index=static_cast<std::int32_t>(signed_value(12));
                t.exit_bar_index=static_cast<std::int32_t>(signed_value(13));t.open_at_end=std::get<bool>(cells[14]);
                w.write_t(signed_value(0),t,std::get<std::uint64_t>(cells[15]),signed_value(16),
                          string_value(17),string_value(18),string_value(19)); break;
            }
            case 'O': {
                const double values[]={float_value(3),float_value(4)};
                w.write_o(signed_value(0),signed_value(1),signed_value(2),values,2);break;
            }
            case 'V': {
                pf_output_event_v1_t e{};e.struct_version=1;e.size=sizeof(e);
                e.sequence=std::get<std::uint64_t>(cells[0]);e.output_index=static_cast<std::int32_t>(signed_value(1));
                e.bar_index=static_cast<std::int32_t>(signed_value(2));e.bar_open_ms=signed_value(3);e.bar_close_ms=signed_value(4);
                e.ordinal_in_bar=static_cast<std::uint32_t>(signed_value(5));e.phase=static_cast<std::uint32_t>(signed_value(6));
                e.confirmed=static_cast<std::uint32_t>(signed_value(7));e.value=float_value(8);
                e.message_hash64=std::get<std::uint64_t>(cells[9]);
                std::string_view message;
                if(!fixture::null(cells[10])) {e.message=string_value(10).c_str();message=string_value(10);}
                w.write_v(e,message);break;
            }
            case 'K': {const double values[]={float_value(0),float_value(1)};w.write_k(values,2);break;}
            case 'X': {
                const pf_trace_entry_t x{signed_value(0),static_cast<std::int32_t>(signed_value(1)),0,float_value(3)};
                w.write_x(x,string_value(2));break;
            }
            case 'H':w.write_h(std::get<std::uint64_t>(cells[0]));break;
            default:throw std::runtime_error("v1 test tag");
            }
        }
        trailer=w.write_trailer();
    }
    std::ifstream file(path.path,std::ios::binary);
    const std::string expected{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    require(!expected.empty(),"v1 writer produced no reference");
    fixture::Writer w;auto r=open(w.encode(input));
    std::string output;
    const auto conversion=r.to_v1([&](std::string_view bytes){output.append(bytes);return true;});
    require(conversion.scope==reader::VerificationScope::whole_file && output==expected,"byte-identical v1 sink");
    require(conversion.v1_sha256==trailer.sha256_hex && conversion.bytes==expected.size(),"v1 receipt digest/length");
    require(r.content_sha256()==trailer.sha256_hex,"content hash excludes v1 trailer");
    std::string partial;unsigned calls=0;
    const auto cancelled=r.to_v1([&](std::string_view bytes){if(++calls==3)return false;partial.append(bytes);return true;});
    require(cancelled.scope==reader::VerificationScope::cancelled && cancelled.v1_sha256.empty()
        && partial.find("\"trailer\"")==std::string::npos,"cancelled export emitted success trailer");
}

void test_source_and_cancellation() {
    fixture::Writer w;w.all=false;
    const auto b=w.encode({equity(100,1.0,0.0),equity(200,2.0,0.0)});
    {
        auto source=std::make_shared<Source>(b.bytes);auto r=reader::Reader::open(source);
        source->generation_="replacement";
        refuses([&]{(void)r.summary();},"generation change","generation");
    }
    {
        auto source=std::make_shared<Source>(b.bytes);source->change_during_read=true;
        refuses([&]{(void)reader::Reader::open(source);},"generation changed during range","generation");
    }
    {
        auto source=std::make_shared<Source>(b.bytes);source->short_read=true;
        refuses([&]{(void)reader::Reader::open(source);},"short range","short readRange");
    }
    {
        auto source=std::make_shared<Source>(b.bytes);bool abort=false;
        auto r=reader::Reader::open(source,{"",[&]{return abort;}});
        abort=true;
        require(r.verify().scope==reader::VerificationScope::cancelled,"explicit cancelled verification");
        abort=false;require(!r.verified().value,"cancelled verification marked complete");
        unsigned called=0;
        const auto result=r.verify([&](const auto&){++called;return false;});
        require(result.scope==reader::VerificationScope::cancelled && called==1 && !r.verified().value,"synchronous callback backpressure");
        require(r.verify().scope==reader::VerificationScope::whole_file,"retry after cancellation");
    }
    {
        auto source=std::make_shared<Source>(b.bytes);auto r=reader::Reader::open(source);
        auto bytes=source->bytes_;
        const auto offset=fixture::get(b.descriptors[0][0],0,8);
        // Unseen column corruption does not turn an opened root into a whole-file receipt.
        source->bytes_[static_cast<std::size_t>(offset)+176]^=1;
        source->generation_="corrupt-generation";
        refuses([&]{(void)r.verify();},"cache bound to generation","generation");
        source->bytes_=std::move(bytes);
    }
}

reader::MemoryStats memory_case(std::size_t n,bool merged) {
    fixture::Writer w;w.all=false;w.caps[0]=8;w.caps[7]=16;
    std::vector<fixture::Row> rows(n,equity(100,1.0,0.0));
    const auto b=w.encode(rows);
    auto source=std::make_shared<Source>(b.bytes);auto r=reader::Reader::open(source);
    require(r.memory_stats().value.live_decoded_blocks==0 && r.memory_stats().value.live_index_pages==0,"cold allocations");
    std::size_t count=0;reader::Record row;
    if(merged) {
        auto cursor=r.iter_merged();while(cursor.next(row))++count;
    } else {
        auto cursor=r.iter_type('E');while(cursor.next(row))++count;
    }
    require(count==n,"memory scan not exercised");
    const auto stats=r.memory_stats().value;
    const auto active=merged?2u:1u; // only E and I have blocks in this input
    require(stats.peak_index_pages<=reader::layout::page_cache,"two-page live-buffer ceiling");
    require(stats.peak_decoded_blocks<=active && stats.live_decoded_blocks==0,"one decoded block per active cursor");
    const std::size_t per_block=3*reader::layout::max_block
        +reader::layout::max_columns*(sizeof(reader::Column)+sizeof(std::string));
    require(stats.peak_decoded_bytes<=active*per_block,"typed buffer/object ceiling derived from hard caps");
    require(stats.peak_column_scratch_bytes<=2*reader::layout::max_column,"column scratch ceiling");
    for(const auto& request:source->requests) require(request.second<=reader::layout::max_head+4
        && request.second!=b.bytes.size(),"whole-file range residency");
    return stats;
}
void test_memory() {
    for(const bool merged:{false,true}) {
        // Identical block caps/value widths; 10x rows and 10x E descriptors.
        // Both sizes cross table boundaries; the large one spans >2 E pages.
        const auto small=memory_case(960,merged),large=memory_case(9600,merged);
        require(large.peak_decoded_blocks==small.peak_decoded_blocks,"block count scales with file");
        require(large.peak_decoded_bytes==small.peak_decoded_bytes,"decoded allocations scale with file");
        require(large.peak_column_scratch_bytes==small.peak_column_scratch_bytes,"scratch scales with file");
        require(large.peak_index_pages<=reader::layout::page_cache,"index pages scale with file");
    }
}

void test_zstd_frames() {
    fixture::Writer w;w.all=false;
    // Raw and RLE+raw frames are hand framed from RFC 8878; no test compressor.
    w.change_column=[](char tag,std::size_t c,fixture::Encoding& e){
        if(tag!='E'||c==0)return;
        const auto raw=e.bytes;
        std::string frame("\x28\xb5\x2f\xfd\x20",5);frame+=fixture::little(raw.size(),1);
        if(c==2) {
            frame+=fixture::little((8u<<3)|2,3);frame+='\0'; // nonlast RLE, eight zeros
            frame+=fixture::little(((raw.size()-8)<<3)|1,3);frame+=raw.substr(8);
        } else {frame+=fixture::little((raw.size()<<3)|1,3);frame+=raw;}
        e.codec=1;e.bytes=frame;
    };
    const std::vector<fixture::Row> input{equity(100,1.0,0.0),equity(200,2.0,0.0)};
    auto r=open(w.encode(input));
    require(r.verify().scope==reader::VerificationScope::whole_file,"raw/RLE zstd frames");
    const auto rows=all_records(r,'E');
    for(std::size_t j=0;j<input.size();++j)equal(rows[j],input[j]);
}
} // namespace

int main() {
    try {
        require(fixture::checksum("123456789")==UINT32_C(0xcbf43926),"independent CRC check value");
        test_summary_preallocation();
        test_summary_tiny_refusals();
        if (!reader::Reader::has_zstd()) {
            bool refused=false;
            try {
                fixture::Writer w; w.all=false;
                (void)open(w.encode({}));
            } catch (const reader::ReaderError& error) {
                require(error.failure()==reader::Failure::unsupported_codec
                    && std::string(error.what()).find("SUMMARY")!=std::string::npos,
                    "decoder-disabled build must refuse mandatory SUMMARY at open");
                refused=true;
            }
            require(refused,"mandatory SUMMARY silently decoded without libzstd");
            std::cerr<<"SKIP [77] SUMMARY decoder disabled; preallocation and unsupported-codec refusals checked; "
                "no decode/query/conformance evidence\n";
            return 77;
        }
        test_summary_frames();
        test_accessor_file_state();
        test_compact_unverified_identity();
        test_records_and_columns();
        test_additional_modes();
        for (const bool compact : {false,true}) {
            test_seek_slice_rank(compact);
            test_envelope(compact);
        }
        test_profiles();
        test_worked_discovery();
        test_profile_bounds_and_presence();
        test_sha_summary_and_order();
        test_report_object();
        test_refusals();
        test_v1_conversion();
        test_source_and_cancellation();
        test_memory();
        test_zstd_frames();
        std::cout<<"L21C independent fixtures, queries, refusals, v1 bytes and memory bounds completed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"L21C reader failure: "<<e.what()<<'\n';return 1;
    }
}
