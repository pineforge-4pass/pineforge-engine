// L21C: consume the Python lane's bundle without calling its encoder/reader.
#include "../tools/sink_v2/report_sink_reader.hpp"
#include "../src/sha256.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace v2 = pineforge::report_sink::v2;
void require(bool ok,const std::string& why) {if(!ok)throw std::runtime_error(why);}
std::string read(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary);require(bool(f),"cannot read "+path.string());
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
int digit(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
v2::Bytes unhex(const std::string& input) {
    v2::Bytes bytes;std::istringstream lines(input);std::string line;int high=-1;
    while(std::getline(lines,line))for(char c:line) {
        if(c=='#')break;
        if(std::isspace(static_cast<unsigned char>(c)))continue;
        const auto value=digit(c);require(value>=0,"invalid vector hex");
        if(high<0)high=value;
        else{bytes.push_back(static_cast<std::uint8_t>(16*high+value));high=-1;}
    }
    require(high<0,"odd vector hex");return bytes;
}
class Source final:public v2::RandomAccessSource {
public:
    using v2::RandomAccessSource::readRange;
    explicit Source(v2::Bytes bytes):bytes_(std::move(bytes)){}
    std::uint64_t size()const override{return bytes_.size();}
    std::string generation()const override{return "immutable-vector";}
    v2::Bytes readRange(std::uint64_t offset,std::size_t length)override {
        require(offset<=bytes_.size()&&length<=bytes_.size()-offset,"vector source range");
        const auto begin=bytes_.begin()+static_cast<std::ptrdiff_t>(offset);
        return {begin,begin+static_cast<std::ptrdiff_t>(length)};
    }
private:
    const v2::Bytes bytes_; // test-owned fixture storage; reader never receives the whole vector
};

// Independent JSON-array value parser. Compare values, not JSON spelling:
// generated records may use \n while v1's canonical string uses \u000a.
class Expected {
public:
    explicit Expected(std::string_view line):line_(line){}
    v2::Record parse(const v2::Head& head,std::array<std::uint64_t,7>& ordinals) {
        punct('[');const auto tag=string();
        require(tag.size()==1,"expected record tag");
        const auto t=std::string_view("ETOVKXH").find(tag[0]);require(t<7,"expected tag domain");
        require(head.records[t].present,"expected tag absent from validated head");
        v2::Record row;row.tag=tag[0];row.ordinal=ordinals[t]++;
        for(const auto& field:head.records[t].fields) {
            punct(',');space();v2::Value value;value.kind=field.kind;
            if(token("null")) {require(field.nullable,"expected nonnullable null");value.null=true;}
            else {
                value.null=false;
                if(field.kind==v2::Kind::string)value.text=string();
                else if(field.kind==v2::Kind::hex64) {
                    const auto text=string();require(text.size()==16,"expected hex width");
                    const auto result=std::from_chars(text.data(),text.data()+text.size(),value.word,16);
                    require(result.ec==std::errc{}&&result.ptr==text.data()+text.size(),"expected hex");
                } else if(field.kind==v2::Kind::boolean) {
                    if(token("true"))value.word=1;
                    else require(token("false"),"expected bool");
                } else {
                    const auto number=numeric();
                    if(field.kind==v2::Kind::f64) {
                        double d=0;const auto result=std::from_chars(number.data(),number.data()+number.size(),d);
                        require(result.ec==std::errc{}&&result.ptr==number.data()+number.size()&&std::isfinite(d),"expected f64");
                        std::memcpy(&value.word,&d,sizeof(d));
                    } else if(field.kind==v2::Kind::i64) {
                        std::int64_t n=0;const auto result=std::from_chars(number.data(),number.data()+number.size(),n);
                        require(result.ec==std::errc{}&&result.ptr==number.data()+number.size(),"expected i64");
                        value.word=static_cast<std::uint64_t>(n);
                    } else {
                        const auto result=std::from_chars(number.data(),number.data()+number.size(),value.word);
                        require(result.ec==std::errc{}&&result.ptr==number.data()+number.size(),"expected u64");
                    }
                }
            }
            row.values.push_back(std::move(value));
        }
        punct(']');space();require(at_==line_.size(),"extra expected record fields");return row;
    }
private:
    std::string_view line_;std::size_t at_=0;
    void space(){while(at_<line_.size()&&std::isspace(static_cast<unsigned char>(line_[at_])))++at_;}
    char take(){require(at_<line_.size(),"short expected JSON");return line_[at_++];}
    void punct(char c){space();require(take()==c,"expected JSON punctuation");}
    bool token(std::string_view text) {
        space();if(line_.substr(at_,text.size())!=text)return false;at_+=text.size();return true;
    }
    unsigned hex4() {
        unsigned value=0;for(unsigned i=0;i<4;++i){const auto d=digit(take());require(d>=0,"expected Unicode escape");value=value*16+static_cast<unsigned>(d);}
        return value;
    }
    static void scalar(std::string& s,unsigned cp) {
        if(cp<128)s+=static_cast<char>(cp);
        else if(cp<2048){s+=static_cast<char>(192|(cp>>6));s+=static_cast<char>(128|(cp&63));}
        else if(cp<65536){s+=static_cast<char>(224|(cp>>12));s+=static_cast<char>(128|((cp>>6)&63));s+=static_cast<char>(128|(cp&63));}
        else{s+=static_cast<char>(240|(cp>>18));s+=static_cast<char>(128|((cp>>12)&63));s+=static_cast<char>(128|((cp>>6)&63));s+=static_cast<char>(128|(cp&63));}
    }
    std::string string() {
        punct('"');std::string s;
        for(;;) {
            const auto c=take();if(c=='"')return s;
            if(c!='\\'){s+=c;continue;}
            switch(take()) {
            case '"':s+='"';break;case '\\':s+='\\';break;case '/':s+='/';break;
            case 'b':s+='\b';break;case 'f':s+='\f';break;case 'n':s+='\n';break;case 'r':s+='\r';break;case 't':s+='\t';break;
            case 'u': {
                auto cp=hex4();
                if(cp>=0xd800&&cp<=0xdbff) {
                    require(take()=='\\'&&take()=='u',"expected paired surrogate");const auto low=hex4();
                    require(low>=0xdc00&&low<=0xdfff,"expected low surrogate");cp=65536+((cp-0xd800)<<10)+low-0xdc00;
                }
                scalar(s,cp);break;
            }
            default:throw std::runtime_error("unknown expected escape");
            }
        }
    }
    std::string_view numeric() {
        space();const auto begin=at_;
        while(at_<line_.size()&&line_[at_]!=','&&line_[at_]!=']'&&!std::isspace(static_cast<unsigned char>(line_[at_])))++at_;
        require(at_>begin,"expected numeric token");return line_.substr(begin,at_-begin);
    }
};
void equal(const v2::Record& a,const v2::Record& b) {
    require(a.tag==b.tag&&a.ordinal==b.ordinal&&a.values.size()==b.values.size(),"vector record shape/order");
    for(std::size_t c=0;c<a.values.size();++c) {
        const auto& x=a.values[c];const auto& y=b.values[c];
        require(x.kind==y.kind&&x.null==y.null,"vector kind/null");
        if(!x.null)require(x.word==y.word&&x.text==y.text,"vector exact value/signed zero/UTF-8");
    }
}
std::string skip_reason(const std::filesystem::path& directory,const std::string& name,const std::string& readme) {
    if(readme.find("STATUS: STALE FULL-FILE VECTORS")!=std::string::npos)
        return "bundle README marks the full-file layout STALE; current FULL/COMPACT CRC/SHA/footer generation pending";
    for(const auto& suffix:{".status",".mask.json"}) {
        const auto path=directory/(name+suffix);
        if(!std::filesystem::exists(path))continue;
        const auto status=read(path);
        const auto status_key=status.find("\"status\"");
        const auto colon=status_key==std::string::npos?std::string::npos:status.find(':',status_key);
        const auto begin=colon==std::string::npos?std::string::npos:status.find('"',colon);
        const auto end=begin==std::string::npos?std::string::npos:status.find('"',begin+1);
        const auto marker=begin==std::string::npos||end==std::string::npos?std::string{}:status.substr(begin+1,end-begin-1);
        if((std::string_view(suffix)==".status"&&(status.find("PENDING")!=std::string::npos||status.find("STALE")!=std::string::npos))
            || marker.find("PENDING")!=std::string::npos || marker=="STALE")
            return path.filename().string()+" marks this vector STALE/PENDING";
    }
    std::istringstream lines(readme);std::string line;
    while(std::getline(lines,line))if((line.find("`"+name+"`")!=std::string::npos||line.find("**"+name+"**")!=std::string::npos
        ||line.find("| "+name+" |")!=std::string::npos)&&
        (line.find("PENDING")!=std::string::npos||line.find("**STALE**")!=std::string::npos))
        return "README marks "+name+" STALE/PENDING";
    return {};
}
} // namespace

int main(int argc, char** argv) {
    try {
#ifdef PF_SINK_V2_VECTOR_DIR
        const auto default_directory=std::filesystem::path(PF_SINK_V2_VECTOR_DIR);
#else
        const auto default_directory=std::filesystem::path(__FILE__).parent_path().parent_path()/"docs/design/sink-v2-vectors";
#endif
        require(argc==1 || argc==2,"usage: test_report_sink_reader_vectors [generated-directory]");
        const auto directory=argc==2 ? std::filesystem::path(argv[1]) : default_directory;
        require(std::filesystem::exists(directory/"GENERATION.json"),"no real-transcoder generation manifest");
        const auto readme=read(directory/"README.md");
        std::vector<std::string> names;
        for(const auto& file:std::filesystem::directory_iterator(directory)) {
            const auto name=file.path().filename().string();
            if(name.size()>7&&name.substr(name.size()-7)==".v2.hex")names.push_back(name.substr(0,name.size()-7));
        }
        std::sort(names.begin(),names.end());require(!names.empty(),"no vector bundle files");
        std::size_t checked=0,skipped=0;
        for(const auto& name:names) {
            const auto reason=skip_reason(directory,name,readme);
            if(!reason.empty()){std::cerr<<"SKIP [77] "<<name<<": "<<reason<<"; skip is not conformance evidence\n";++skipped;continue;}
            try {
                auto r=v2::Reader::open(std::make_shared<Source>(unhex(read(directory/(name+".v2.hex")))));
                require(r.verify().scope==v2::VerificationScope::whole_file,"vector incomplete verification");
                auto cursor=r.iter_merged();v2::Record actual;std::vector<v2::Record> expected;
                std::array<std::uint64_t,7> ordinals{};std::istringstream lines(read(directory/(name+".records.jsonl")));std::string line;
                while(std::getline(lines,line)) {
                    require(!line.empty(),"blank expected record line");
                    auto row=Expected(line).parse(r.head().value,ordinals);
                    require(cursor.next(actual),"missing vector record");equal(actual,row);expected.push_back(std::move(row));
                }
                require(!cursor.next(actual),"extra vector record");
                for(std::size_t t=0;t<7;++t)if(r.head().value.records[t].present) {
                    auto per_type=r.iter_type(std::string_view("ETOVKXH")[t]);
                    for(const auto& row:expected)if(row.tag==std::string_view("ETOVKXH")[t]) {
                        require(per_type.next(actual),"missing per-type vector row");equal(actual,row);
                    }
                    require(!per_type.next(actual),"extra per-type vector row");
                }
                std::ifstream v1(directory/(name+".v1.jsonl"),std::ios::binary);
                require(bool(v1),"missing valid v1 reference");
                const auto converted=r.to_v1([&](std::string_view chunk) {
                    std::string reference(chunk.size(),'\0');v1.read(reference.data(),static_cast<std::streamsize>(reference.size()));
                    require(static_cast<std::size_t>(v1.gcount())==chunk.size()&&reference==chunk,"vector v1 byte equivalence");return true;
                });
                require(converted.scope==v2::VerificationScope::whole_file&&v1.peek()==std::char_traits<char>::eof(),"vector v1 completion/extra bytes");
                const auto report=read(directory/(name+".report.json"));
                const bool typed_refusal=report.find("\"code\":\"report_result_invalid\"")!=std::string::npos;
                std::size_t at=0;
                pineforge::report_sink::Sha256 report_hash;report_hash.update(report);
                try {
                    const auto result=r.to_report_json([&](std::string_view bytes) {
                        require(!typed_refusal,"typed-refusal vector emitted report bytes");
                        require(at<=report.size() && bytes.size()<=report.size()-at && report.compare(at,bytes.size(),bytes)==0,
                                "vector report-object byte equivalence");
                        at+=bytes.size();return true;
                    });
                    require(!typed_refusal && result.scope==v2::VerificationScope::whole_file && at==report.size()
                        && result.bytes==report.size() && result.sha256==report_hash.hex_digest(),
                        "vector report-object LF/length/SHA completion");
                } catch(const v2::ReaderError& error) {
                    require(typed_refusal && at==0 && error.failure()==v2::Failure::report_result_invalid,
                            "wrong vector report-object typed refusal");
                    const auto field_at=std::string(error.what()).find("field=");
                    const auto end=std::string(error.what()).find(" reason=",field_at);
                    require(field_at!=std::string::npos && end!=std::string::npos
                        && report.find(std::string(error.what()).substr(field_at+6,end-field_at-6))!=std::string::npos
                        && report.find("nonfinite")!=std::string::npos,"vector refusal field/reason sidecar");
                }
                std::cout<<"decoded and compared every record: "<<name<<'\n';++checked;
            } catch(const v2::ReaderError& e) {
                if(e.failure()!=v2::Failure::unsupported_codec)throw;
                std::cerr<<"SKIP [77] "<<name<<": "<<e.what()<<"; no zstd conformance evidence\n";++skipped;
            }
        }
        std::cout<<"vector files verified="<<checked<<" skipped="<<skipped<<'\n';
        // Never report a green conformance row when any requested fixture is pending.
        return skipped?77:0;
    } catch(const std::exception& e) {
        std::cerr<<"L21C vector failure: "<<e.what()<<'\n';return 1;
    }
}
