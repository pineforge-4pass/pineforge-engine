#pragma once

#include "sink_v2_reader_fixtures.hpp"

namespace sink_v2_reader_fixture {

// Replay the DATA negatives still exercised verbatim in test_refusals(), plus
// the CRC-valid/SHA-invalid selected-column row and dictionary-order proof.
// The probe sends each control/defect through full verify and incremental
// replay. The old reader suite retains its assertions verbatim; both
// executables use Writer from the shared header, with no second encoder.
struct DataCase {
    using Defect = std::function<void(Writer&, std::vector<Row>&)>;
    std::string name, diagnostic;
    Writer control;
    std::vector<Row> input;
    Defect defect;
    bool integrity = false;
    std::string incremental_diagnostic;

    Built build(bool faulty) const {
        auto writer = control;
        auto rows = input;
        writer.export_replay = true;
        if (faulty) defect(writer, rows);
        return writer.encode(rows);
    }
};

inline std::vector<DataCase> data_cases() {
    std::vector<DataCase> cases;
    auto add = [&](std::string name, std::string diagnostic, const Writer& control,
                   const std::vector<Row>& input, DataCase::Defect defect,
                   bool integrity = false, std::string incremental = "") {
        cases.push_back({std::move(name), std::move(diagnostic), control, input,
                         std::move(defect), integrity, std::move(incremental)});
    };
    Writer ordinary; ordinary.all = false;
    const std::vector<Row> small{equity(100,10.0,1.0), equity(200,11.0,2.0)};
    Writer typed;
    const auto all = population();
    {
        auto bitmap = ordinary;
        bitmap.change_column = [](char tag, std::size_t c, Encoding& e) {
            if (tag == 'E' && c == 1) { e.flags = 1; e.bytes.insert(0,1,'\0'); }
        };
        add("unused null bitmap bits", "nonzero unused null bits", bitmap, small,
            [](Writer& w, auto&) {
                w.change_column = [](char tag, std::size_t c, Encoding& e) {
                    if (tag == 'E' && c == 1) { e.flags=1; e.bytes.insert(0,1,static_cast<char>(0x80)); }
                };
            });
    }
    add("nonfinite f64", "non-finite stored f64", ordinary, small, [](Writer& w, auto&) {
        w.change_column = [](char tag, std::size_t c, Encoding& e) {
            if (tag=='E' && c==1) { e.bytes.assign(16,'\0'); e.bytes[12]=static_cast<char>(0xf0); e.bytes[14]=0x7f; }
        };
    });
    const std::vector<std::pair<std::string, std::string>> deltas{
        {std::string("\x80\0\1",3), "nonminimal/out-of-domain varint"},
        {std::string(9,static_cast<char>(0x80)), "varint exceeds eight bytes"},
        {uv(UINT64_C(9007199254740992)), "nonminimal/out-of-domain varint"},
        {uv(200)+uv(0)+uv(2), "zero DELTA GCD"},
        {uv(0)+uv(UINT64_C(9007199254740991))+uv(UINT64_C(4503599627370496)), "delta multiplication overflow"},
        {uv(0)+uv(1)+uv(UINT64_C(9007199254740991)), "delta quotient outside domain"}};
    auto delta = ordinary;
    delta.change_column = [](char tag, std::size_t c, Encoding& e) {
        if (tag=='E' && c==0) { e.mode=1; e.bytes=uv(200)+uv(100)+uv(2); }
    };
    for (std::size_t n=0; n<deltas.size(); ++n) {
        const auto body = deltas[n].first;
        add("DELTA "+std::to_string(n), deltas[n].second, delta, small, [body](Writer& w, auto&) {
            w.change_column = [body](char tag, std::size_t c, Encoding& e) {
                if (tag=='E' && c==0) { e.mode=1; e.bytes=body; }
            };
        });
    }
    auto decimal = ordinary;
    decimal.change_column = [](char tag, std::size_t c, Encoding& e) {
        if (tag=='E' && c==1) { e.mode=3; e.bytes=std::string(1,'\0')+uv(20)+uv(1)+uv(2); }
    };
    const std::vector<std::pair<std::string, std::string>> decimals{
        {std::string(1,'\15')+uv(2)+uv(1)+uv(0), "DECIMAL scale exceeds 12"},
        {std::string(1,'\0')+uv(UINT64_C(2251799813685248))+uv(1)+uv(0), "DECIMAL integer outside domain"}};
    for (std::size_t n=0; n<decimals.size(); ++n) {
        const auto body = decimals[n].first;
        add("DECIMAL "+std::to_string(n), decimals[n].second, decimal, small, [body](Writer& w, auto&) {
            w.change_column = [body](char tag, std::size_t c, Encoding& e) {
                if (tag=='E' && c==1) { e.mode=3; e.bytes=body; }
            };
        });
    }
    auto framed = ordinary;
    framed.change_column = [](char tag, std::size_t c, Encoding& e) {
        if (tag=='E' && c==0) { e.codec=1; e.bytes=raw_frame(e.bytes); }
    };
    const std::array<const char*,5> frame_diagnostics{{
        "zstd window exceeds cap before allocation", "zstd frame has no declared content size",
        "zstd dictionary/reserved bits forbidden", "zstd content size differs from raw_len",
        "trailing/concatenated zstd frame"}};
    for (unsigned fault=0; fault<frame_diagnostics.size(); ++fault) {
        add("zstd framing "+std::to_string(fault), frame_diagnostics[fault], framed, small,
            [fault](Writer& w, auto&) {
                w.change_column = [fault](char tag, std::size_t c, Encoding& e) {
                    if (tag!='E' || c!=0) return;
                    auto frame=raw_frame(e.bytes);
                    if (fault==0) frame=std::string("\x28\xb5\x2f\xfd\x80\x78",6)+little(e.bytes.size(),4);
                    if (fault==1) frame=std::string("\x28\xb5\x2f\xfd\0\0",6);
                    if (fault==2) frame[4]=0x21;
                    if (fault==3) frame[5]^=1;
                    if (fault==4) frame+='\0';
                    e.codec=1; e.bytes=std::move(frame);
                };
            });
    }
    add("bool", "invalid bool byte", typed, all, [](Writer& w, auto&) {
        w.change_column=[](char tag, std::size_t c, Encoding& e) { if(tag=='T' && c==14) e.bytes[0]=2; };
    });
    for (const auto c : {15u,16u}) {
        add("unsafe integer "+std::to_string(c), c==15 ? "u64 outside safe domain" : "i64 outside safe domain",
            typed, all, [c](Writer& w, auto&) {
                w.change_column=[c](char tag, std::size_t col, Encoding& e) {
                    if(tag=='T' && col==c) e.bytes.replace(0,8,little(c==15?UINT64_MAX:UINT64_C(0x8000000000000000),8));
                };
            });
    }
    add("invalid UTF-8", "invalid UTF-8", typed, all, [](Writer& w, auto&) {
        w.change_column=[](char tag, std::size_t c, Encoding& e) {
            if(tag=='T' && c==17) e.bytes=std::string("\1\1\xc0\xaf",4);
        };
    });
    add("dictionary index", "dictionary index outside range", typed, all, [](Writer& w, auto&) {
        w.change_column=[](char tag, std::size_t c, Encoding& e) {
            if(tag=='T' && c==17) { e.mode=5; e.bytes=uv(1)+uv(1)+"a"+uv(1)+uv(0); }
        };
    });
    {
        // Same decoded strings and column CRC/SHA on both sides. Only the
        // dictionary's first-occurrence order differs.
        auto rows = std::vector<Row>{trade(1,1.0), trade(2,2.0)};
        rows[0].values[17]=s("b"); rows[1].values[17]=s("a");
        auto dictionary = ordinary;
        dictionary.change_column=[](char tag, std::size_t c, Encoding& e) {
            if(tag=='T' && c==17) { e.mode=5; e.bytes=uv(2)+uv(1)+uv(1)+"ba"+uv(0)+uv(1); }
        };
        add("dictionary order", "dictionary not in first-occurrence order", dictionary, rows,
            [](Writer& w, auto&) {
                w.change_column=[](char tag, std::size_t c, Encoding& e) {
                    if(tag=='T' && c==17) { e.mode=5; e.bytes=uv(2)+uv(1)+uv(1)+"ab"+uv(1)+uv(0); }
                };
            });
    }
    add("trailing string body", "unused dictionary/trailing string bytes", typed, all, [](Writer& w, auto&) {
        w.change_column=[](char tag, std::size_t c, Encoding& e) { if(tag=='T' && c==17) e.bytes+='x'; };
    });
    add("side enum", "invalid T.side", typed, all, [](auto&, auto& rows) { rows[1].values[1]=s("neither"); });
    add("decreasing E", "E time decreases", ordinary, small, [](auto&, auto& rows) {
        rows[0].values[0]=i(200); rows[1].values[0]=i(100);
    });
    add("T.n order", "T.n is not ordinal+1", typed, all, [](auto&, auto& rows) { rows[1].values[0]=i(2); });
    add("O.row order", "O.row is not ordinal", typed, all, [](auto&, auto& rows) { rows[2].values[0]=i(1); });
    add("I multiplicities", "I multiplicities mismatch", typed, all, [](Writer& w, auto&) {
        w.change_order=[](char tag) { return tag=='T' ? 'E' : tag; };
    }, false, "incremental input I order mismatch");
    add("decoded CRC", "decoded column CRC mismatch", ordinary, small, [](Writer& w, auto&) {
        w.change_directory=[](char tag, auto& dir) { if(tag=='E') dir[12]^=1; };
    }, true);
    add("flipped stored column byte", "decoded column CRC mismatch", ordinary, small, [](Writer& w, auto&) {
        w.change_column=[](char tag, std::size_t c, Encoding& e) { if(tag=='E' && c==0) e.bytes[0]^=1; };
    }, true);
    add("CRC-valid/SHA-invalid column", "decoded column SHA mismatch", ordinary,
        {equity(100,1.0,0.0), equity(200,2.0,0.0)}, [](Writer& w, auto&) {
            w.change_column_shas=[](char tag, auto& shas) { if(tag=='E') shas[32]^=1; };
        }, true);
    // Directory admission remains late too, but replay its existing negatives
    // so malformed framing cannot masquerade as a DATA-class success.
    const std::array<const char*,5> directory_diagnostics{{
        "unknown codec/directory reserved", "encoding not allowed for kind", "invalid null bitmap flags",
        "unknown codec/directory reserved", "column length exceeds cap"}};
    for (unsigned fault=0; fault<directory_diagnostics.size(); ++fault) {
        add("directory "+std::to_string(fault), directory_diagnostics[fault], ordinary, small,
            [fault](Writer& w, auto&) {
                w.change_directory=[fault](char tag, auto& dir) {
                    if(tag!='E') return;
                    if(fault==0) dir[0]=2;
                    if(fault==1) dir[1]=7;
                    if(fault==2) dir[2]=1;
                    if(fault==3) dir[3]=1;
                    if(fault==4) put(dir,4,16777217,4);
                };
            });
    }
    {
        Writer budget;
        budget.slots=600; budget.caps[2]=65536; budget.caps[7]=65536;
        Row output{'O',{i(0),std::monostate{},std::monostate{}}};
        output.values.insert(output.values.end(),600,Cell(0.0));
        add("decoded budget before allocation", "decoded U budget before allocation", budget, {output},
            [](Writer& w, auto&) {
                w.change_bhead=[](char tag, auto& h) { if(tag=='O') put(h,12,65536,4); };
                w.change_descriptor=[](char tag, std::uint64_t, auto& d) { if(tag=='O') put(d,16,65536,4); };
                w.change_root=[](auto& root) { put(root,48,65536,8); put(root,88,65536,8); };
                w.replay_admissions=65536;
            });
    }
    return cases;
}

} // namespace sink_v2_reader_fixture
