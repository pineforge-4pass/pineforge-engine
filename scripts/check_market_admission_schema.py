#!/usr/bin/env python3
"""Static completeness of canonical market-admission storage/reflection.

Reads declarations and explicit reflection folds; does not execute engine code.
The expected schema is a checked contract, not discovered silently at generation.
"""
from pathlib import Path
import json
import re
import sys

ROOT=Path(__file__).resolve().parents[1]
def stripped(text):
    return re.sub(r'//[^\n]*|/\*.*?\*/','',text,flags=re.S)
def body(text,pattern,label):
    matches=list(re.finditer(pattern,text))
    if len(matches)!=1:raise ValueError(label+': expected one definition')
    start=matches[0].end();depth=1
    for i in range(start,len(text)):
        depth+=(text[i]=='{')-(text[i]=='}')
        if depth==0:return text[start:i]
    raise ValueError(label+': unclosed body')
def storage(text,name):
    value=body(text,r'\b(?:struct|class)\s+'+name+r'\s*\{',name)
    result={};statement='';i=0
    while i<len(value):
        c=value[i]
        if c=='{':
            if '(' not in statement:raise ValueError(name+': unknown braced storage')
            depth=1;i+=1
            while i<len(value) and depth:
                depth+=(value[i]=='{')-(value[i]=='}');i+=1
            statement='';continue
        if c==';':
            decl=' '.join(statement.split());statement=''
            if not decl or decl.startswith('using ') or decl.startswith('friend class '):i+=1;continue
            if '(' in decl:
                if '(*' in decl:raise ValueError(name+': unclassified function-pointer storage')
                i+=1;continue
            m=re.fullmatch(r'(.+?) (\w+)(?: = .*)?',decl)
            if not m or m[2] in result:raise ValueError(name+': unclassified declaration '+decl)
            result[m[2]]=m[1]
        else:
            statement+=c
            if statement.strip() in ['public:','private:','protected:']:statement=''
        i+=1
    if statement.strip():raise ValueError(name+': trailing declaration')
    return result

def order_fields(schema):
    values=[]; appended=[]
    types={'uint64_t':'uint64_t','int64_t':'int64_t','int':'int64_t','bool':'uint64_t','double':'double','std::string':'std::string','CommandKind':'int64_t','Checkpoint':'int64_t'}
    def leaf(p,t):
        # The original 72 admission leaves already belong to the aggregate
        # mirror. New target identities append after its full existing suffix.
        target=appended if p in ['review.target_command','sizing_revision.target_command'] else values
        target.append([p.replace('.','_'),t,'draft.'+p])
    def walk(name,prefix):
        for n,t in schema[name].items():
            path=prefix+'.'+n
            if t in types:leaf(path,types[t])
            elif t=='OrderBirth':
                for n,t in [('cause','int64_t'),('bar','int64_t'),('timestamp','int64_t'),('cursor_domain','int64_t'),('cursor_position','int64_t'),('cursor_index','int64_t'),('cursor_count','int64_t'),('cursor_price','double'),('first_fill','uint64_t'),('last_fill','uint64_t'),('evaluation_ordinal','uint64_t')]:leaf(path+'.'+n,t)
            elif t=='std::optional<SizingObservation>':leaf(prefix+'.original_sizing_present','uint64_t');walk('SizingObservation',path)
            else:walk(t,path)
    leaf('observation_present','uint64_t');walk('CommandObservation','observation')
    leaf('review_present','uint64_t');walk('ReviewReceipt','review')
    leaf('sizing_revision_present','uint64_t');walk('SizingRevision','sizing_revision')
    return values+appended

def check(root=ROOT):
    header=stripped((root/'include/pineforge/market_admission.hpp').read_text())
    source=stripped((root/'src/market_admission.cpp').read_text())
    hash_source=stripped((root/'src/engine_state_hash.cpp').read_text())
    sink_header=root/'src/broker_state_hash_internal.hpp'
    if sink_header.is_file():
        hash_source += '\n' + stripped(sink_header.read_text())
    source_hash_path=root/'src/source/pine_state_hash.cpp'
    if source_hash_path.is_file():
        hash_source += '\n' + stripped(source_hash_path.read_text())
    schema=json.loads((root/'scripts/market_admission_schema.json').read_text())
    expected_names={'Configuration','PriceRequest','CurrentPrices','SizingObservation','CommandObservation','ReviewReceipt','SizingRevision','Draft','BookObservation','CommandEvent','InstructionResolution','ReviewEvent','SizingEvent','Journal'}
    if set(schema)!=expected_names:raise ValueError('market admission canonical type schema changed')
    for name,fields in schema.items():
        if storage(header,name)!=fields:raise ValueError(name+': every canonical stored field must be classified')
    enums={'CommandKind':['Entry','Raw','Cancel','CancelAll'],
           'Outcome':['Admitted','NoAdmission','IgnoredTradingWindow','IgnoredIntradayLoss','RejectedIntradayCap','RejectedFrozenMarketCap','RejectedAffordability','RejectedPricedCap','OpeningRejectedReductionAdmitted','CancelCompleted'],
           'Checkpoint':['DefaultGross','ExplicitPair','TerminalGross'],
           'ResolutionKind':['Original','Rejected','PairedTransaction']}
    for name,values in enums.items():
        actual=[x.strip() for x in body(header,r'enum class '+name+r'\s*:\s*int64_t\s*\{',name).split(',') if x.strip()]
        if actual!=values:raise ValueError(name+': domain discriminator changed')
    compact=lambda s:re.sub(r'\s+','',s)
    for declaration in ['using Event = std::variant<CommandEvent, ReviewEvent, SizingEvent>;', 'using FieldValue = std::variant<uint64_t, int64_t, double, std::string>;']:
        if compact(declaration) not in compact(header):raise ValueError('admission variant alternatives changed')
    specs={'Configuration':'config','SizingObservation':'sizing','CommandObservation':'command','ReviewReceipt':'review','SizingRevision':'revision','BookObservation':'book','InstructionResolution':'resolution'}
    blocks={name:body(source,r'void '+fn+r'\(const '+name+r'& o,const std::string& p\)const\s*\{',fn) for name,fn in specs.items()}
    for name,fields in schema.items():
        if name not in blocks:continue
        block=compact(blocks[name])
        for field,kind in fields.items():
            if kind in ['Configuration','OrderBirth','CurrentPrices','Draft','PriceRequest','std::optional<SizingObservation>']:continue
            # Macro is tied to an actual typed leaf emission, not a name-only read.
            folded=('F('+field+');' in block and '#defineF(name)field(p,#name,o.name)' in block) or ('field(p,"'+field+'",o.'+field+');' in block)
            if not folded:raise ValueError(name+'.'+field+': missing actual-value reflection')
    required=[
        'birth(o.birth,p+".birth");','config(o.configuration,p+".configuration");',
        'field(p+".prices","limit",o.prices.limit);','field(p+".prices","stop",o.prices.stop);',
        'field(p+".prices","trail_points",o.prices.trail_points);','field(p+".prices","trail_price",o.prices.trail_price);','field(p+".prices","trail_offset",o.prices.trail_offset);',
        'field(p,"original_sizing_present",o.original_sizing.has_value());','if(o.original_sizing)sizing(*o.original_sizing,p+".original_sizing");',
        'field(p,"observation_present",bool(o.observation()));','if(o.observation())command(*o.observation(),p+".observation");',
        'field(p,"review_present",o.review().has_value());','if(o.review())review(*o.review(),p+".review");',
        'field(p,"sizing_revision_present",o.sizing_revision().has_value());','if(o.sizing_revision())revision(*o.sizing_revision(),p+".sizing_revision");',
        'draft(o.draft,p+".draft");','field(p,"kind",uint64_t(value.index()));',
        'command(*o.observation,p+".observation");','field(p,"outcome",o.outcome);','field(p,"admitted_incarnation",o.admitted_incarnation);',
        'array(o.before,p+".before",[&](const auto& x,const auto& q){book(x,q);});',
        'array(o.removed,p+".removed",[&](auto x,const auto& q){field(q,"incarnation",x);});',
        'review(o.receipt,p+".receipt");','field(p,"open_price",o.open_price);','field(p,"position_side",o.position_side);','field(p,"position_cycle",o.position_cycle);',
        'array(o.book,p+".book",[&](const auto& x,const auto& q){book(x,q);});',
        'array(o.reviewed,p+".reviewed",[&](const auto& x,const auto& q){book(x,q);});',
        'array(o.resolutions,p+".resolutions",[&](const auto& x,const auto& q){resolution(x,q);});',
        'array(o.causes,p+".causes",[&](auto x,const auto& q){field(q,"sequence",x);});',
        'revision(o.receipt,p+".receipt");','field(p,"incarnation",o.incarnation);','sizing(o.before,p+".before");','sizing(o.after,p+".after");',
        'field(p,"affordability_equity_before",o.affordability_equity_before);','field(p,"affordability_equity_after",o.affordability_equity_after);',
        'field(p,"size",uint64_t(values.size()));','r.field(path,"next_sequence",next_sequence_);',
        'r.field(path,"active_allocations",active_allocations_);',
        'r.array(outstanding_sequences_,path+".outstanding_sequences",[&](auto sequence,const auto& p){r.field(p,"sequence",sequence);});',
        'r.array(events_,path+".events",[&](const auto& event,const auto& p){r.event(event,p);});']
    for fold in required:
        if compact(fold) not in compact(source):raise ValueError('missing nested admission reflection: '+fold)
    birth_block=body(source,r'void birth\(const OrderBirth& o,const std::string& p\)const\s*\{','birth')
    for field,expr in [('cause','cause()'),('bar','bar()'),('timestamp','timestamp()'),('cursor_domain','cursor().domain()'),('cursor_position','cursor().position()'),('cursor_index','cursor().index()'),('cursor_count','cursor().count()'),('cursor_price','cursor_price()'),('first_fill','first_fill()'),('last_fill','last_fill()'),('evaluation_ordinal','evaluation_ordinal()')]:
        if compact(f'field(p,"{field}",o.{expr});') not in compact(birth_block):raise ValueError('incomplete admission birth reflection: '+field)
    mirrors=json.loads((root/'scripts/market_admission_mirror_fields.json').read_text())
    if mirrors!=order_fields(schema):raise ValueError('admission C mirror must reflect every actual per-order fact')
    folds=[
        ('admission::reflect(o.market_admission,"draft",[&](const auto& field){hash_admission_field(f,field);});',),
        ('market_admission_journal_.reflect("journal",[&](const auto& field){hash_admission_field(f,field);});',
         'adapter_.admission_journal.reflect("journal",[&](const auto& field){hash_admission_field(f,field);});'),
        ('f.s(field.path);f.u(field.value.index());',),
    ]
    for alternatives in folds:
        if not any(compact(fold) in compact(hash_source) for fold in alternatives):
            raise ValueError('admission actual-value hash plumbing missing')
    for path in ['scripts/broker_state_hash_waivers.txt','scripts/pending_order_mirror_waivers.txt']:
        for line in (root/path).read_text().splitlines():
            if line.split('#',1)[0].strip() and 'market_admission' in line.split('#',1)[0]:raise ValueError('market admission cannot be waived')
    return len(mirrors)
if __name__=='__main__':
    try: print(f'market admission: canonical storage, variants, typed reflection and {check()} per-order leaves covered')
    except (ValueError,OSError) as error:print(error,file=sys.stderr);raise SystemExit(1)
