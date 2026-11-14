#include <astra/assets/gltf.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <variant>

namespace astra::assets {
namespace {

struct Json final {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value{nullptr};
    [[nodiscard]] const Json* get(std::string_view key) const {
        const auto* object = std::get_if<Object>(&value);
        if (!object) return nullptr;
        const auto it = object->find(key);
        return it == object->end() ? nullptr : &it->second;
    }
    [[nodiscard]] const Json* at(std::size_t index) const {
        const auto* array = std::get_if<Array>(&value);
        return array && index < array->size() ? &(*array)[index] : nullptr;
    }
    [[nodiscard]] std::size_t size() const {
        if (const auto* a = std::get_if<Array>(&value)) return a->size();
        if (const auto* o = std::get_if<Object>(&value)) return o->size();
        return 0;
    }
    [[nodiscard]] std::string string(std::string fallback = {}) const {
        if (const auto* s = std::get_if<std::string>(&value)) return *s;
        return fallback;
    }
    [[nodiscard]] double number(double fallback = 0.0) const {
        if (const auto* n = std::get_if<double>(&value)) return *n;
        return fallback;
    }
    [[nodiscard]] bool boolean(bool fallback = false) const {
        if (const auto* b = std::get_if<bool>(&value)) return *b;
        return fallback;
    }
};

class JsonParser final {
  public:
    JsonParser(std::string_view input, std::size_t max_depth) : input_(input), max_depth_(max_depth) {}
    Json parse() {
        skip(); Json result = value(0); skip();
        if (position_ != input_.size()) fail("trailing characters after JSON document");
        return result;
    }
  private:
    std::string_view input_; std::size_t position_{}; std::size_t max_depth_;
    [[noreturn]] void fail(const char* message) const { throw std::runtime_error(std::string(message) + " at JSON byte " + std::to_string(position_)); }
    void skip() { while (position_ < input_.size() && (input_[position_]==' ' || input_[position_]=='\n' || input_[position_]=='\r' || input_[position_]=='\t')) ++position_; }
    char take() { if (position_ >= input_.size()) fail("unexpected end of JSON"); return input_[position_++]; }
    bool consume(std::string_view token) { if (input_.substr(position_, token.size()) != token) return false; position_ += token.size(); return true; }
    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7ff) { out.push_back(static_cast<char>(0xc0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 63))); }
        else if (cp <= 0xffff) { out.push_back(static_cast<char>(0xe0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63))); out.push_back(static_cast<char>(0x80 | (cp & 63))); }
        else { out.push_back(static_cast<char>(0xf0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63))); out.push_back(static_cast<char>(0x80 | (cp & 63))); }
    }
    std::uint32_t hex4() { std::uint32_t n=0; for(int i=0;i<4;++i){ char c=take(); n<<=4; if(c>='0'&&c<='9')n|=c-'0';else if(c>='a'&&c<='f')n|=c-'a'+10;else if(c>='A'&&c<='F')n|=c-'A'+10;else fail("invalid unicode escape"); } return n; }
    std::string string_value() {
        if (take()!='"') fail("expected string"); std::string out;
        while (position_<input_.size()) { char c=take(); if(c=='"') return out; if(static_cast<unsigned char>(c)<0x20) fail("control character in string");
            if(c!='\\'){out.push_back(c);continue;} c=take();
            switch(c){case '"':case '\\':case '/':out.push_back(c);break;case 'b':out.push_back('\b');break;case 'f':out.push_back('\f');break;case 'n':out.push_back('\n');break;case 'r':out.push_back('\r');break;case 't':out.push_back('\t');break;
            case 'u': { auto cp=hex4(); if(cp>=0xd800&&cp<=0xdbff){ if(take()!='\\'||take()!='u')fail("invalid surrogate pair"); auto low=hex4(); if(low<0xdc00||low>0xdfff)fail("invalid surrogate pair"); cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00); } else if(cp>=0xdc00&&cp<=0xdfff) fail("unpaired low surrogate"); utf8(out,cp); break; }
            default:fail("invalid string escape");}
        } fail("unterminated string");
    }
    Json value(std::size_t depth) {
        if(depth>max_depth_) fail("JSON nesting limit exceeded"); skip(); if(position_>=input_.size()) fail("missing JSON value");
        const char c=input_[position_];
        if(c=='"'){Json j;j.value=string_value();return j;}
        if(c=='{'){++position_;Json::Object o;skip();if(position_<input_.size()&&input_[position_]=='}'){++position_;Json j;j.value=std::move(o);return j;} for(;;){skip();if(position_>=input_.size()||input_[position_]!='"')fail("object key must be a string");auto k=string_value();skip();if(take()!=':')fail("expected colon");auto [it,inserted]=o.emplace(std::move(k),value(depth+1));if(!inserted)fail("duplicate object key");skip();char end=take();if(end=='}')break;if(end!=',')fail("expected comma in object");}Json j;j.value=std::move(o);return j;}
        if(c=='['){++position_;Json::Array a;skip();if(position_<input_.size()&&input_[position_]==']'){++position_;Json j;j.value=std::move(a);return j;}for(;;){a.push_back(value(depth+1));skip();char end=take();if(end==']')break;if(end!=',')fail("expected comma in array");}Json j;j.value=std::move(a);return j;}
        if(consume("true")){Json j;j.value=true;return j;}if(consume("false")){Json j;j.value=false;return j;}if(consume("null"))return {};
        const auto begin=position_;if(input_[position_]=='-')++position_;if(position_>=input_.size())fail("invalid number");if(input_[position_]=='0')++position_;else{if(input_[position_]<'1'||input_[position_]>'9')fail("invalid value");while(position_<input_.size()&&input_[position_]>='0'&&input_[position_]<='9')++position_;}if(position_<input_.size()&&input_[position_]=='.'){++position_;auto p=position_;while(position_<input_.size()&&input_[position_]>='0'&&input_[position_]<='9')++position_;if(p==position_)fail("invalid fraction");}if(position_<input_.size()&&(input_[position_]=='e'||input_[position_]=='E')){++position_;if(position_<input_.size()&&(input_[position_]=='+'||input_[position_]=='-'))++position_;auto p=position_;while(position_<input_.size()&&input_[position_]>='0'&&input_[position_]<='9')++position_;if(p==position_)fail("invalid exponent");}
        const std::string token(input_.substr(begin,position_-begin));std::istringstream number_stream(token);number_stream.imbue(std::locale::classic());double n{};number_stream>>n;if(!number_stream||number_stream.peek()!=std::char_traits<char>::eof()||!std::isfinite(n))fail("invalid or non-finite number");Json j;j.value=n;return j;
    }
};

const Json& required(const Json& j, std::string_view key) { const auto* p=j.get(key); if(!p)throw std::runtime_error("missing required property '"+std::string(key)+"'");return *p; }
std::uint32_t u32(const Json& j, const char* label) { const double n=j.number(-1);if(n<0||n>std::numeric_limits<std::uint32_t>::max()||std::floor(n)!=n)throw std::runtime_error(std::string("invalid ")+label);return static_cast<std::uint32_t>(n); }
std::uint32_t index(const Json& j, std::string_view key, std::uint32_t fallback=~std::uint32_t{0}) {const auto*p=j.get(key);return p?u32(*p,std::string(key).c_str()):fallback;}
float number(const Json& j,float fallback=0){const auto n=j.number(fallback);if(!std::isfinite(n)||std::abs(n)>std::numeric_limits<float>::max())throw std::runtime_error("number is outside float range");return static_cast<float>(n);}
std::vector<float> floats(const Json* j,std::size_t count,float fallback=0){std::vector<float> r(count,fallback);if(!j)return r;if(j->size()!=count)throw std::runtime_error("array has an invalid number of components");for(std::size_t i=0;i<count;++i)r[i]=number(*j->at(i));return r;}
std::vector<std::byte> read_bytes(const std::filesystem::path& path,std::size_t limit){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("cannot open resource: "+path.string());auto end=f.tellg();if(end<0||static_cast<std::uintmax_t>(end)>limit||static_cast<std::uintmax_t>(end)>std::numeric_limits<std::size_t>::max()||static_cast<std::uintmax_t>(end)>static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))throw std::runtime_error("resource exceeds configured size limit: "+path.string());std::vector<std::byte>b(static_cast<std::size_t>(end));f.seekg(0);if(!b.empty()&&!f.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size())))throw std::runtime_error("failed reading resource: "+path.string());return b;}
std::vector<std::byte> base64(std::string_view s){
    auto decode=[](unsigned char c)->int{if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;};
    std::vector<std::byte> out;unsigned acc=0,bits=0;bool padded=false;unsigned padding=0;
    for(char raw:s){const auto c=static_cast<unsigned char>(raw);if(c==' '||c=='\n'||c=='\r'||c=='\t')continue;if(c=='='){padded=true;++padding;if(padding>2)throw std::runtime_error("invalid base64 padding");continue;}const int digit=decode(c);if(padded||digit<0)throw std::runtime_error("invalid base64 data URI");acc=(acc<<6U)|static_cast<unsigned>(digit);bits+=6;if(bits>=8){bits-=8;out.push_back(static_cast<std::byte>((acc>>bits)&255U));}}
    if(bits>=6||(padding&&((padding==1&&bits!=2)||(padding==2&&bits!=4))))throw std::runtime_error("invalid base64 padding");return out;}
std::vector<std::byte> data_uri(std::string_view uri,std::string* mime=nullptr){auto comma=uri.find(',');if(!uri.starts_with("data:")||comma==std::string_view::npos)throw std::runtime_error("invalid data URI");auto meta=uri.substr(5,comma-5);auto semi=meta.find(';');if(mime)*mime=std::string(meta.substr(0,semi));auto payload=uri.substr(comma+1);if(meta.ends_with(";base64"))return base64(payload);std::vector<std::byte> out;for(std::size_t i=0;i<payload.size();++i){unsigned char c=payload[i];if(c=='%'){if(i+2>=payload.size())throw std::runtime_error("invalid percent escape in data URI");auto hex=[](char x)->int{if(x>='0'&&x<='9')return x-'0';if(x>='a'&&x<='f')return x-'a'+10;if(x>='A'&&x<='F')return x-'A'+10;return -1;};int a=hex(payload[++i]),b=hex(payload[++i]);if(a<0||b<0)throw std::runtime_error("invalid percent escape in data URI");c=static_cast<unsigned char>((a<<4)|b);}out.push_back(static_cast<std::byte>(c));}return out;}
std::string decode_uri_path(std::string_view uri){std::string out;auto hex=[](char x)->int{if(x>='0'&&x<='9')return x-'0';if(x>='a'&&x<='f')return x-'a'+10;if(x>='A'&&x<='F')return x-'A'+10;return -1;};for(std::size_t i=0;i<uri.size();++i){if(uri[i]=='%'){if(i+2>=uri.size())throw std::runtime_error("invalid percent escape in resource URI");int a=hex(uri[++i]),b=hex(uri[++i]);if(a<0||b<0)throw std::runtime_error("invalid percent escape in resource URI");out.push_back(static_cast<char>((a<<4)|b));}else out.push_back(uri[i]);}return out;}

struct BufferView{std::uint32_t buffer{};std::size_t offset{},length{},stride{};};
struct Accessor{std::uint32_t view{~std::uint32_t{0}};std::size_t offset{},count{};std::uint32_t component{};std::string type;bool normalized{};const Json* sparse{};};
struct Context{
    const Json& root; Asset& asset; std::vector<std::vector<std::byte>> buffers;std::vector<BufferView> views;std::vector<Accessor> accessors;std::size_t max_bytes;mutable std::size_t resource_bytes{};
    void consume_bytes(std::size_t bytes,const char*what)const{if(resource_bytes>max_bytes||bytes>max_bytes-resource_bytes)throw std::runtime_error(std::string(what)+" exceed aggregate resource size limit");resource_bytes+=bytes;}
    std::span<const std::byte> view_data(std::uint32_t i)const{if(i>=views.size())throw std::runtime_error("bufferView index out of range");const auto&v=views[i];if(v.buffer>=buffers.size()||v.offset>buffers[v.buffer].size()||v.length>buffers[v.buffer].size()-v.offset)throw std::runtime_error("bufferView exceeds its buffer");return std::span<const std::byte>(buffers[v.buffer]).subspan(v.offset,v.length);}
    std::size_t component_size(std::uint32_t c)const{switch(c){case 5120:case 5121:return 1;case 5122:case 5123:return 2;case 5125:case 5126:return 4;default:throw std::runtime_error("unsupported accessor componentType");}}
    std::size_t components(std::string_view t)const{if(t=="SCALAR")return 1;if(t=="VEC2")return 2;if(t=="VEC3")return 3;if(t=="VEC4")return 4;if(t=="MAT2")return 4;if(t=="MAT3")return 9;if(t=="MAT4")return 16;throw std::runtime_error("unsupported accessor type");}
    std::size_t element_size(std::string_view type,std::size_t component_bytes)const{if(type.starts_with("MAT")){const auto dimension=static_cast<std::size_t>(type.back()-'0');const auto column_bytes=dimension*component_bytes;const auto column_stride=(column_bytes+3U)&~std::size_t{3U};return dimension*column_stride;}return components(type)*component_bytes;}
    std::size_t component_offset(std::string_view type,std::size_t component,std::size_t component_bytes)const{if(!type.starts_with("MAT"))return component*component_bytes;const auto dimension=static_cast<std::size_t>(type.back()-'0');const auto column_stride=(dimension*component_bytes+3U)&~std::size_t{3U};return (component/dimension)*column_stride+(component%dimension)*component_bytes;}
    double scalar(const std::byte*p,std::uint32_t c,bool norm)const{switch(c){case 5120:{std::int8_t v;std::memcpy(&v,p,1);return norm?std::max(-1.0,static_cast<double>(v)/127.0):v;}case 5121:{std::uint8_t v;std::memcpy(&v,p,1);return norm?static_cast<double>(v)/255.0:v;}case 5122:{std::int16_t v;std::memcpy(&v,p,2);return norm?std::max(-1.0,static_cast<double>(v)/32767.0):v;}case 5123:{std::uint16_t v;std::memcpy(&v,p,2);return norm?static_cast<double>(v)/65535.0:v;}case 5125:{std::uint32_t v;std::memcpy(&v,p,4);return norm?static_cast<double>(v)/4294967295.0:v;}case 5126:{float v;std::memcpy(&v,p,4);if(!std::isfinite(v))throw std::runtime_error("non-finite float in accessor");return v;}default:throw std::runtime_error("unsupported accessor componentType");}}
    std::vector<float> read_accessor(std::uint32_t ai,std::size_t expected=0)const{
        if(ai>=accessors.size())throw std::runtime_error("accessor index out of range");const auto&a=accessors[ai];const auto n=components(a.type),cs=component_size(a.component),element=element_size(a.type,cs);if(expected&&n!=expected)throw std::runtime_error("accessor has the wrong component count");if(a.count>max_bytes/(std::max<std::size_t>(n,1)*sizeof(float)))throw std::runtime_error("accessor allocation exceeds size limit");const auto output_bytes=a.count*n*sizeof(float);consume_bytes(output_bytes,"decoded accessors");std::vector<float> out(a.count*n,0.0F);
        if(a.normalized&&(a.component==5126||a.component==5125))throw std::runtime_error("normalized accessors require an 8-bit or 16-bit integer componentType");
        if(a.view!=~std::uint32_t{0}){auto data=view_data(a.view);const auto&v=views[a.view];const auto stride=v.stride?v.stride:element;const bool matrix=a.type.starts_with("MAT");if((v.offset+a.offset)%cs||stride%cs||a.offset%cs||(matrix&&(v.offset+a.offset)%4))throw std::runtime_error("accessor data is not component-aligned");if(stride<element||a.offset>data.size()||(a.count&&((a.count-1)>((data.size()-a.offset)/stride)||element>data.size()-a.offset-(a.count-1)*stride)))throw std::runtime_error("accessor range exceeds bufferView");for(std::size_t i=0;i<a.count;++i)for(std::size_t c=0;c<n;++c)out[i*n+c]=static_cast<float>(scalar(data.data()+a.offset+i*stride+component_offset(a.type,c,cs),a.component,a.normalized));}
        if(a.sparse){const auto&sp=*a.sparse;const auto count=u32(required(sp,"count"),"sparse count");if(count>a.count)throw std::runtime_error("sparse accessor count exceeds accessor count");const auto&ix=required(sp,"indices");const auto&vv=required(sp,"values");const auto iv=index(ix,"bufferView"),vv_i=index(vv,"bufferView");const auto ic=index(ix,"componentType");if(ic!=5121&&ic!=5123&&ic!=5125)throw std::runtime_error("invalid sparse index componentType");auto id=view_data(iv),vd=view_data(vv_i);if(views[iv].stride||views[vv_i].stride)throw std::runtime_error("sparse accessor bufferViews cannot define byteStride");const auto io=ix.get("byteOffset")?u32(*ix.get("byteOffset"),"sparse index offset"):0;const auto vo=vv.get("byteOffset")?u32(*vv.get("byteOffset"),"sparse value offset"):0;const auto is=component_size(ic);const auto&index_view=views[iv];const auto&value_view=views[vv_i];if((index_view.offset+io)%is||(value_view.offset+vo)%cs||(a.type.starts_with("MAT")&&(value_view.offset+vo)%4)||io>id.size()||count>(id.size()-io)/is||vo>vd.size()||count>(vd.size()-vo)/element)throw std::runtime_error("sparse accessor range or alignment is invalid");std::size_t previous=0;for(std::size_t k=0;k<count;++k){auto dst=static_cast<std::size_t>(scalar(id.data()+io+k*is,ic,false));if(dst>=a.count||(k&&dst<=previous))throw std::runtime_error("sparse accessor indices must be in-range and strictly increasing");previous=dst;for(std::size_t c=0;c<n;++c)out[dst*n+c]=static_cast<float>(scalar(vd.data()+vo+k*element+component_offset(a.type,c,cs),a.component,a.normalized));}}
        return out;
    }
};

std::uint32_t optional_u32(const Json& j, std::string_view k, std::uint32_t fallback = 0) {
    auto* p = j.get(k);
    return p ? u32(*p, std::string(k).c_str()) : fallback;
}
Vec3 vec3(const Json* j, Vec3 d = {}) {
    if (!j)
        return d;
    auto v = floats(j, 3);
    return {v[0], v[1], v[2]};
}
Vec2 vec2(const Json* j, Vec2 d = {}) {
    if (!j)
        return d;
    auto v = floats(j, 2);
    return {v[0], v[1]};
}
Vec4 vec4(const Json* j, Vec4 d = {}) {
    if (!j)
        return d;
    auto v = floats(j, 4);
    return {v[0], v[1], v[2], v[3]};
}
TextureInfo texture_info(const Json* j, bool occlusion = false) {
    TextureInfo t;
    if (j) {
        t.texture = index(*j, "index");
        t.texcoord = optional_u32(*j, "texCoord");
        const auto* strength = j->get(occlusion ? "strength" : "scale");
        t.scale = number(strength ? *strength : Json{}, 1.0F);
        if (const auto* extensions = j->get("extensions")) {
            if (const auto* transform = extensions->get("KHR_texture_transform")) {
                t.uv_offset = vec2(transform->get("offset"));
                t.uv_scale = vec2(transform->get("scale"), {1.0F, 1.0F});
                t.uv_rotation = number(transform->get("rotation") ? *transform->get("rotation")
                                                                   : Json{});
                if (const auto* texcoord = transform->get("texCoord"))
                    t.texcoord = u32(*texcoord, "KHR_texture_transform texCoord");
            }
        }
    }
    return t;
}

Sampler parse_sampler(const Json& value) {
    Sampler result;
    const auto mag = optional_u32(value, "magFilter", 9729);
    if (mag == 9728)
        result.mag_filter = TextureFilter::nearest;
    else if (mag == 9729)
        result.mag_filter = TextureFilter::linear;
    else
        throw std::runtime_error("invalid glTF sampler magFilter");
    const auto min = optional_u32(value, "minFilter", 9987);
    switch (min) {
    case 9728:
        result.min_filter = TextureFilter::nearest;
        result.mip_filter = TextureMipFilter::none;
        break;
    case 9729:
        result.min_filter = TextureFilter::linear;
        result.mip_filter = TextureMipFilter::none;
        break;
    case 9984:
        result.min_filter = TextureFilter::nearest;
        result.mip_filter = TextureMipFilter::nearest;
        break;
    case 9985:
        result.min_filter = TextureFilter::linear;
        result.mip_filter = TextureMipFilter::nearest;
        break;
    case 9986:
        result.min_filter = TextureFilter::nearest;
        result.mip_filter = TextureMipFilter::linear;
        break;
    case 9987:
        result.min_filter = TextureFilter::linear;
        result.mip_filter = TextureMipFilter::linear;
        break;
    default:
        throw std::runtime_error("invalid glTF sampler minFilter");
    }
    const auto parse_wrap = [&](const char* key) {
        switch (optional_u32(value, key, 10497)) {
        case 10497:
            return TextureWrap::repeat;
        case 33648:
            return TextureWrap::mirrored_repeat;
        case 33071:
            return TextureWrap::clamp_to_edge;
        default:
            throw std::runtime_error(std::string("invalid glTF sampler ") + key);
        }
    };
    result.wrap_s = parse_wrap("wrapS");
    result.wrap_t = parse_wrap("wrapT");
    return result;
}

Asset load(const std::filesystem::path& path,LoadOptions options){
    auto file=read_bytes(path,options.maximum_file_bytes);std::string json;std::vector<std::byte> glb_bin;
    const bool glb=path.extension()==".glb"||(file.size()>=4&&std::memcmp(file.data(),"glTF",4)==0);
    if(glb){if(file.size()<12||std::memcmp(file.data(),"glTF",4)!=0)throw std::runtime_error("invalid GLB header");auto read32=[&](std::size_t o){std::uint32_t v;std::memcpy(&v,file.data()+o,4);return v;};if(read32(4)!=2)throw std::runtime_error("only GLB version 2 is supported");if(read32(8)!=file.size())throw std::runtime_error("GLB declared length does not match file size");std::size_t pos=12;bool got_json=false;while(pos<file.size()){if(file.size()-pos<8)throw std::runtime_error("truncated GLB chunk header");auto len=read32(pos),type=read32(pos+4);pos+=8;if(len%4)throw std::runtime_error("GLB chunk length is not 4-byte aligned");if(len>file.size()-pos)throw std::runtime_error("GLB chunk exceeds file size");if(type==0x4e4f534a){if(got_json||pos!=20)throw std::runtime_error("GLB JSON chunk must be first and unique");json.assign(reinterpret_cast<const char*>(file.data()+pos),len);while(!json.empty()&&(json.back()=='\0'||json.back()==' '))json.pop_back();got_json=true;}else if(type==0x004e4942){if(!got_json||!glb_bin.empty())throw std::runtime_error("GLB BIN chunk is misplaced or duplicated");glb_bin.assign(file.begin()+static_cast<std::ptrdiff_t>(pos),file.begin()+static_cast<std::ptrdiff_t>(pos+len));}pos+=len;}if(!got_json)throw std::runtime_error("GLB has no JSON chunk");}
    else json.assign(reinterpret_cast<const char*>(file.data()),file.size());
    Json root=JsonParser(json,options.maximum_json_depth).parse();const auto version=required(required(root,"asset"),"version").string();if(!version.starts_with("2."))throw std::runtime_error("document is not glTF 2.x");
    bool mesh_quantization = false;
    if (const auto* extensions = root.get("extensionsUsed")) {
        for (std::size_t i = 0; i < extensions->size(); ++i) {
            if (extensions->at(i)->string() == "KHR_mesh_quantization")
                mesh_quantization = true;
        }
    }
    if (const auto* extensions = root.get("extensionsRequired")) {
        for (std::size_t i = 0; i < extensions->size(); ++i) {
            const auto extension = extensions->at(i)->string();
            if (extension != "KHR_mesh_quantization")
                throw std::runtime_error("document requires unsupported glTF extension: " + extension);
            mesh_quantization = true;
        }
    }
    Asset asset;asset.source=path;Context c{root,asset,{}, {}, {},options.maximum_file_bytes};const auto base=path.parent_path();
    if(const auto*arr=root.get("buffers")){for(std::size_t i=0;i<arr->size();++i){const auto&b=*arr->at(i);const auto length=u32(required(b,"byteLength"),"buffer byteLength");std::vector<std::byte> bytes;if(const auto*u=b.get("uri")){auto uri=u->string();bytes=uri.starts_with("data:")?data_uri(uri):read_bytes(base/std::filesystem::path(decode_uri_path(uri)),options.maximum_file_bytes);}else if(i==0&&!glb_bin.empty())bytes=glb_bin;else throw std::runtime_error("buffer has no URI or GLB BIN chunk");if(bytes.size()<length)throw std::runtime_error("buffer is shorter than declared byteLength");c.consume_bytes(bytes.size(),"buffer resources");c.buffers.push_back(std::move(bytes));}}
    if(const auto*arr=root.get("bufferViews")){for(std::size_t i=0;i<arr->size();++i){const auto&v=*arr->at(i);BufferView x;x.buffer=index(v,"buffer");x.offset=optional_u32(v,"byteOffset");x.length=u32(required(v,"byteLength"),"bufferView byteLength");x.stride=optional_u32(v,"byteStride");if(x.buffer>=c.buffers.size()||x.offset>c.buffers[x.buffer].size()||x.length>c.buffers[x.buffer].size()-x.offset)throw std::runtime_error("bufferView is outside its buffer");if(x.stride&&(x.stride<4||x.stride>252||x.stride%4))throw std::runtime_error("invalid bufferView byteStride");c.views.push_back(x);}}
    if(const auto*arr=root.get("accessors")){for(std::size_t i=0;i<arr->size();++i){const auto&a=*arr->at(i);Accessor x;x.view=index(a,"bufferView");x.offset=optional_u32(a,"byteOffset");x.count=u32(required(a,"count"),"accessor count");x.component=index(a,"componentType");x.type=required(a,"type").string();x.normalized=a.get("normalized")&&a.get("normalized")->boolean();x.sparse=a.get("sparse");(void)c.component_size(x.component);(void)c.components(x.type);if(x.view==~std::uint32_t{0}&&!x.sparse)throw std::runtime_error("accessor without bufferView must be sparse");c.accessors.push_back(x);}}
    if(const auto*arr=root.get("images")){for(std::size_t i=0;i<arr->size();++i){const auto&im=*arr->at(i);TextureImage out;out.name=im.get("name")?im.get("name")->string():"image_"+std::to_string(i);out.mime_type=im.get("mimeType")?im.get("mimeType")->string():"";if(im.get("uri")&&im.get("bufferView"))throw std::runtime_error("image cannot define both URI and bufferView");if(const auto*u=im.get("uri")){auto uri=u->string();if(uri.starts_with("data:")){std::string mime;out.bytes=data_uri(uri,&mime);if(out.mime_type.empty())out.mime_type=std::move(mime);}else{out.source_path=base/std::filesystem::path(decode_uri_path(uri));out.bytes=read_bytes(out.source_path,options.maximum_file_bytes);}}else if(const auto*v=im.get("bufferView")){auto data=c.view_data(u32(*v,"image bufferView"));out.bytes.assign(data.begin(),data.end());}else throw std::runtime_error("image has neither URI nor bufferView");c.consume_bytes(out.bytes.size(),"image resources");asset.images.push_back(std::move(out));}}
    if (const auto* arr = root.get("samplers")) {
        for (std::size_t i = 0; i < arr->size(); ++i)
            asset.samplers.push_back(parse_sampler(*arr->at(i)));
    }
    if (const auto* arr = root.get("textures")) {
        for (std::size_t i = 0; i < arr->size(); ++i) {
            const auto& t = *arr->at(i);
            Texture x;
            x.name = t.get("name") ? t.get("name")->string() : "texture_" + std::to_string(i);
            x.image = index(t, "source");
            x.sampler = index(t, "sampler");
            if (x.image >= asset.images.size())
                throw std::runtime_error("texture source image index out of range");
            if (x.sampler != ~std::uint32_t{0}) {
                if (x.sampler >= asset.samplers.size())
                    throw std::runtime_error("texture sampler index out of range");
                x.sampling = asset.samplers[x.sampler];
            }
            asset.textures.push_back(std::move(x));
        }
    }
    if (const auto* arr = root.get("materials")) {
        for (std::size_t i = 0; i < arr->size(); ++i) {
            const auto& m = *arr->at(i);
            Material x;
            x.name = m.get("name") ? m.get("name")->string() : "material_" + std::to_string(i);
            if (const auto* p = m.get("pbrMetallicRoughness")) {
                x.base_color_factor = vec4(p->get("baseColorFactor"), x.base_color_factor);
                x.metallic_factor =
                    number(p->get("metallicFactor") ? *p->get("metallicFactor") : Json{}, 1);
                x.roughness_factor =
                    number(p->get("roughnessFactor") ? *p->get("roughnessFactor") : Json{}, 1);
                x.base_color_texture = texture_info(p->get("baseColorTexture"));
                x.metallic_roughness_texture = texture_info(p->get("metallicRoughnessTexture"));
            }
            x.emissive_factor = vec3(m.get("emissiveFactor"));
            x.normal_texture = texture_info(m.get("normalTexture"));
            x.occlusion_texture = texture_info(m.get("occlusionTexture"), true);
            x.emissive_texture = texture_info(m.get("emissiveTexture"));
            auto alpha = m.get("alphaMode") ? m.get("alphaMode")->string("OPAQUE") : "OPAQUE";
            if (alpha == "MASK")
                x.alpha_mode = 1;
            else if (alpha == "BLEND")
                x.alpha_mode = 2;
            else if (alpha != "OPAQUE")
                throw std::runtime_error("invalid material alphaMode");
            x.alpha_cutoff = number(m.get("alphaCutoff") ? *m.get("alphaCutoff") : Json{}, 0.5F);
            x.double_sided = m.get("doubleSided") && m.get("doubleSided")->boolean();
            asset.materials.push_back(std::move(x));
        }
    }
    for (const auto& material : asset.materials) {
        const TextureInfo* infos[] = {
            &material.base_color_texture, &material.metallic_roughness_texture,
            &material.normal_texture, &material.occlusion_texture, &material.emissive_texture};
        for (const auto* info : infos)
            if (info->texture != ~std::uint32_t{0} && info->texture >= asset.textures.size())
                throw std::runtime_error("material texture index out of range");
    }
    if (const auto* arr = root.get("meshes")) {
        for (std::size_t mi = 0; mi < arr->size(); ++mi) {
            const auto& m = *arr->at(mi);
            Mesh mesh;
            mesh.name = m.get("name") ? m.get("name")->string() : "mesh_" + std::to_string(mi);
            const auto* prims = m.get("primitives");
            if (!prims)
                throw std::runtime_error("mesh has no primitives");
            for (std::size_t pi = 0; pi < prims->size(); ++pi) {
                const auto& p = *prims->at(pi);
                if (p.get("targets"))
                    throw std::runtime_error(
                        "morph target primitives are not supported by this loader");
                Primitive prim;
                prim.material = index(p, "material");
                prim.mode = optional_u32(p, "mode", 4);
                if (prim.mode > 6)
                    throw std::runtime_error("invalid primitive mode");
                if (prim.material != ~std::uint32_t{0} && prim.material >= asset.materials.size())
                    throw std::runtime_error("primitive material index out of range");
                const auto& attrs = required(p, "attributes");
                const auto* pos = attrs.get("POSITION");
                if (!pos)
                    throw std::runtime_error("mesh primitive is missing POSITION");
                auto positions = c.read_accessor(u32(*pos, "POSITION accessor"), 3);
                const auto count = positions.size() / 3;
                if (count > options.maximum_file_bytes / sizeof(Vertex))
                    throw std::runtime_error("expanded vertex data exceeds configured size limit");
                c.consume_bytes(count * sizeof(Vertex), "expanded mesh vertices");
                prim.vertices.resize(count);
                for (std::size_t i = 0; i < count; ++i)
                    prim.vertices[i].position = {positions[i * 3], positions[i * 3 + 1],
                                                 positions[i * 3 + 2]};
                auto fill = [&](const char* name, std::size_t n, auto setter) {
                    if (const auto* a = attrs.get(name)) {
                        auto v = c.read_accessor(u32(*a, name), n);
                        if (v.size() / n != count)
                            throw std::runtime_error(std::string(name) +
                                                     " accessor count does not match POSITION");
                        for (std::size_t i = 0; i < count; ++i)
                            setter(prim.vertices[i], v.data() + i * n);
                    }
                };
                auto validate_attribute = [&](const char* name,
                                              std::initializer_list<std::uint32_t> components,
                                              bool normalized_integer_required) {
                    if (const auto* a = attrs.get(name)) {
                        const auto id = u32(*a, name);
                        if (id >= c.accessors.size())
                            throw std::runtime_error(std::string(name) +
                                                     " accessor index out of range");
                        const auto& ac = c.accessors[id];
                        const bool integer_component = ac.component != 5126;
                        if (std::find(components.begin(), components.end(), ac.component) ==
                                components.end() ||
                            (ac.normalized && !normalized_integer_required) ||
                            (normalized_integer_required && integer_component && !ac.normalized))
                            throw std::runtime_error(
                                std::string(name) +
                                " accessor uses an invalid componentType/normalized combination");
                    }
                };
                validate_attribute("POSITION", mesh_quantization ?
                    std::initializer_list<std::uint32_t>{5126, 5120, 5122} :
                    std::initializer_list<std::uint32_t>{5126}, mesh_quantization);
                validate_attribute("NORMAL", mesh_quantization ?
                    std::initializer_list<std::uint32_t>{5126, 5120, 5122} :
                    std::initializer_list<std::uint32_t>{5126}, mesh_quantization);
                validate_attribute("TANGENT", mesh_quantization ?
                    std::initializer_list<std::uint32_t>{5126, 5120, 5122} :
                    std::initializer_list<std::uint32_t>{5126}, mesh_quantization);
                validate_attribute("TEXCOORD_0", {5126, 5121, 5123}, true);
                validate_attribute("TEXCOORD_1", {5126, 5121, 5123}, true);
                validate_attribute("COLOR_0", {5126, 5121, 5123}, true);
                validate_attribute("JOINTS_0", {5121, 5123}, false);
                validate_attribute("WEIGHTS_0", {5126, 5121, 5123}, true);
                if (attrs.get("JOINTS_1") || attrs.get("WEIGHTS_1"))
                    throw std::runtime_error("JOINTS_1/WEIGHTS_1 are not supported; this importer "
                                             "accepts four skin influences");
                if (static_cast<bool>(attrs.get("JOINTS_0")) !=
                    static_cast<bool>(attrs.get("WEIGHTS_0")))
                    throw std::runtime_error("JOINTS_0 and WEIGHTS_0 must be provided together");
                prim.has_joint_attributes = attrs.get("JOINTS_0") != nullptr;
                prim.has_weight_attributes = attrs.get("WEIGHTS_0") != nullptr;
                fill("NORMAL", 3, [](Vertex& v, const float* p) { v.normal = {p[0], p[1], p[2]}; });
                fill("TANGENT", 4,
                     [](Vertex& v, const float* p) { v.tangent = {p[0], p[1], p[2], p[3]}; });
                fill("TEXCOORD_0", 2,
                     [](Vertex& v, const float* p) { v.texcoord0 = {p[0], p[1]}; });
                fill("TEXCOORD_1", 2,
                     [](Vertex& v, const float* p) { v.texcoord1 = {p[0], p[1]}; });
                if (const auto* color = attrs.get("COLOR_0")) {
                    const auto id = u32(*color, "COLOR_0 accessor");
                    if (id >= c.accessors.size())
                        throw std::runtime_error("COLOR_0 accessor index out of range");
                    const auto components = c.components(c.accessors[id].type);
                    if (components != 3 && components != 4)
                        throw std::runtime_error("COLOR_0 accessor must be VEC3 or VEC4");
                    const auto values = c.read_accessor(id);
                    if (values.size() / components != count)
                        throw std::runtime_error("COLOR_0 accessor count does not match POSITION");
                    for (std::size_t i = 0; i < count; ++i)
                        prim.vertices[i].color = {
                            values[i * components], values[i * components + 1],
                            values[i * components + 2],
                            components == 4 ? values[i * components + 3] : 1.0F};
                }
                fill("JOINTS_0", 4, [](Vertex& v, const float* p) {
                    for (std::size_t k = 0; k < 4; ++k) {
                        if (p[k] < 0 || p[k] > 65535 || std::floor(p[k]) != p[k])
                            throw std::runtime_error("joint index outside uint16 range");
                        v.joints[k] = static_cast<std::uint16_t>(p[k]);
                    }
                });
                fill("WEIGHTS_0", 4,
                     [](Vertex& v, const float* p) { v.weights = {p[0], p[1], p[2], p[3]}; });
                if (const auto* idx = p.get("indices")) {
                    const auto id = u32(*idx, "indices accessor");
                    if (id >= c.accessors.size())
                        throw std::runtime_error("indices accessor index out of range");
                    const auto& ac = c.accessors[id];
                    if (ac.type != "SCALAR" ||
                        (ac.component != 5121 && ac.component != 5123 && ac.component != 5125) ||
                        ac.normalized)
                        throw std::runtime_error(
                            "indices accessor must be an unnormalized unsigned scalar");
                    auto values = c.read_accessor(id, 1);
                    if (values.size() > options.maximum_file_bytes / sizeof(std::uint32_t))
                        throw std::runtime_error(
                            "expanded index data exceeds configured size limit");
                    c.consume_bytes(values.size() * sizeof(std::uint32_t), "expanded mesh indices");
                    prim.indices.reserve(values.size());
                    for (float f : values) {
                        if (f < 0 || f >= static_cast<float>(count) || std::floor(f) != f)
                            throw std::runtime_error("primitive index out of range");
                        prim.indices.push_back(static_cast<std::uint32_t>(f));
                    }
                } else {
                    if (count > options.maximum_file_bytes / sizeof(std::uint32_t))
                        throw std::runtime_error(
                            "generated index data exceeds configured size limit");
                    c.consume_bytes(count * sizeof(std::uint32_t), "generated mesh indices");
                    prim.indices.resize(count);
                    for (std::size_t k = 0; k < count; ++k)
                        prim.indices[k] = static_cast<std::uint32_t>(k);
                }
                mesh.primitives.push_back(std::move(prim));
            }
            asset.meshes.push_back(std::move(mesh));
        }
    }
    if (const auto* arr = root.get("nodes")) {
        asset.nodes.resize(arr->size());
        for (std::size_t i = 0; i < arr->size(); ++i) {
            const auto& n = *arr->at(i);
            auto& x = asset.nodes[i];
            x.name = n.get("name") ? n.get("name")->string() : "node_" + std::to_string(i);
            x.mesh = index(n, "mesh");
            x.skin = index(n, "skin");
            x.translation = vec3(n.get("translation"));
            x.rotation = vec4(n.get("rotation"), x.rotation);
            x.scale = vec3(n.get("scale"), x.scale);
            if (const auto* mat = n.get("matrix")) {
                if (n.get("translation") || n.get("rotation") || n.get("scale"))
                    throw std::runtime_error(
                        "node cannot define matrix and TRS transforms together");
                auto v = floats(mat, 16);
                std::copy(v.begin(), v.end(), x.matrix.begin());
                x.has_matrix = true;
            }
            if (const auto* ch = n.get("children")) {
                for (std::size_t k = 0; k < ch->size(); ++k)
                    x.children.push_back(u32(*ch->at(k), "node child"));
            }
        }
        for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
            auto& n = asset.nodes[i];
            if (n.mesh != ~std::uint32_t{0} && n.mesh >= asset.meshes.size())
                throw std::runtime_error("node mesh index out of range");
            for (auto child : n.children) {
                if (child >= asset.nodes.size() || child == i)
                    throw std::runtime_error("invalid node child index");
                if (asset.nodes[child].parent != -1)
                    throw std::runtime_error("node has multiple parents");
                asset.nodes[child].parent = static_cast<std::int32_t>(i);
            }
        }
        std::vector<unsigned char> marks(asset.nodes.size());
        auto visit = [&](auto&& self, std::uint32_t id) -> void {
            if (marks[id] == 1)
                throw std::runtime_error("node hierarchy contains a cycle");
            if (marks[id] == 2)
                return;
            marks[id] = 1;
            for (auto child : asset.nodes[id].children)
                self(self, child);
            marks[id] = 2;
        };
        for (std::size_t i = 0; i < asset.nodes.size(); ++i)
            visit(visit, static_cast<std::uint32_t>(i));
    }
    if (const auto* arr = root.get("skins")) {
        for (std::size_t i = 0; i < arr->size(); ++i) {
            const auto& s = *arr->at(i);
            Skin x;
            x.name = s.get("name") ? s.get("name")->string() : "skin_" + std::to_string(i);
            if (const auto* p = s.get("skeleton")) {
                x.skeleton = static_cast<std::int32_t>(u32(*p, "skin skeleton"));
                if (static_cast<std::size_t>(x.skeleton) >= asset.nodes.size())
                    throw std::runtime_error("skin skeleton node index out of range");
            }
            const auto& j = required(s, "joints");
            for (std::size_t k = 0; k < j.size(); ++k) {
                auto joint = u32(*j.at(k), "skin joint");
                if (joint >= asset.nodes.size())
                    throw std::runtime_error("skin joint node index out of range");
                x.joints.push_back(joint);
            }
            if (const auto* p = s.get("inverseBindMatrices")) {
                const auto id = u32(*p, "inverseBindMatrices accessor");
                if (id >= c.accessors.size() || c.accessors[id].component != 5126 ||
                    c.accessors[id].type != "MAT4")
                    throw std::runtime_error(
                        "inverse bind matrices must use a FLOAT MAT4 accessor");
                auto vals = c.read_accessor(id, 16);
                if (vals.size() != x.joints.size() * 16)
                    throw std::runtime_error("inverse bind matrix count differs from joint count");
                for (std::size_t k = 0; k < x.joints.size(); ++k) {
                    Mat4 m;
                    std::copy_n(vals.begin() + static_cast<std::ptrdiff_t>(k * 16), 16, m.begin());
                    x.inverse_bind_matrices.push_back(m);
                }
            } else
                x.inverse_bind_matrices.resize(
                    x.joints.size(), Mat4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
            asset.skins.push_back(std::move(x));
        }
    }
    for (auto& n : asset.nodes) {
        if (n.skin != ~std::uint32_t{0}) {
            if (n.skin >= asset.skins.size())
                throw std::runtime_error("node skin index out of range");
            if (n.mesh == ~std::uint32_t{0})
                throw std::runtime_error("node with a skin must also reference a mesh");
            const auto& skin = asset.skins[n.skin];
            for (const auto& primitive : asset.meshes[n.mesh].primitives) {
                if (!primitive.has_joint_attributes || !primitive.has_weight_attributes)
                    throw std::runtime_error(
                        "a skinned mesh primitive must provide JOINTS_0 and WEIGHTS_0");
                for (const auto& vertex : primitive.vertices) {
                    const float weights[] = {vertex.weights.x, vertex.weights.y, vertex.weights.z,
                                             vertex.weights.w};
                    for (std::size_t influence = 0; influence < 4; ++influence) {
                        if (weights[influence] < 0.0F)
                            throw std::runtime_error("skin weights must be nonnegative");
                        if (vertex.joints[influence] >= skin.joints.size())
                            throw std::runtime_error(
                                "vertex joint index exceeds the skin joint count");
                    }
                }
            }
        } else if (n.mesh != ~std::uint32_t{0}) {
            for (const auto& primitive : asset.meshes[n.mesh].primitives)
                if (primitive.has_joint_attributes || primitive.has_weight_attributes)
                    throw std::runtime_error(
                        "a mesh with skin attributes must be attached to a node with a skin");
        }
    }
    if (const auto* arr = root.get("animations")) {
        for(std::size_t ai=0;ai<arr->size();++ai){
            const auto&a=*arr->at(ai);Animation x;x.name=a.get("name")?a.get("name")->string():"animation_"+std::to_string(ai);
            const auto&samplers=required(a,"samplers");std::vector<std::uint32_t> sampler_outputs;
            for(std::size_t si=0;si<samplers.size();++si){
                const auto&s=*samplers.at(si);AnimationSampler sm;
                auto interp=s.get("interpolation")?s.get("interpolation")->string("LINEAR"):"LINEAR";
                if(interp=="STEP")sm.interpolation=Interpolation::step;else if(interp=="CUBICSPLINE")sm.interpolation=Interpolation::cubic_spline;else if(interp!="LINEAR")throw std::runtime_error("invalid animation interpolation");
                const auto input=index(s,"input");if(input>=c.accessors.size()||c.accessors[input].component!=5126||c.accessors[input].type!="SCALAR"||c.accessors[input].normalized)throw std::runtime_error("animation input must be an unnormalized FLOAT SCALAR accessor");
                sm.times=c.read_accessor(input,1);for(std::size_t k=0;k<sm.times.size();++k)if(!std::isfinite(sm.times[k])||sm.times[k]<0.0F||(k&&!(sm.times[k]>sm.times[k-1])))throw std::runtime_error("animation key times must be finite, nonnegative, and strictly increasing");
                const auto output=index(s,"output");if(output>=c.accessors.size()||c.accessors[output].component!=5126||c.accessors[output].normalized)throw std::runtime_error("animation output must use unnormalized FLOAT values");
                sampler_outputs.push_back(output);sm.components=static_cast<std::uint32_t>(c.components(c.accessors[output].type));sm.values=c.read_accessor(output);x.duration=std::max(x.duration,sm.times.empty()?0.0F:sm.times.back());x.samplers.push_back(std::move(sm));
            }
            const auto&channels=required(a,"channels");
            for(std::size_t ci=0;ci<channels.size();++ci){
                const auto&ch=*channels.at(ci);AnimationChannel out;out.sampler=index(ch,"sampler");if(out.sampler>=x.samplers.size())throw std::runtime_error("animation sampler index out of range");
                const auto&t=required(ch,"target");out.node=index(t,"node");if(out.node>=asset.nodes.size())throw std::runtime_error("animation target node index out of range");if(asset.nodes[out.node].has_matrix)throw std::runtime_error("animation target node must use TRS properties, not matrix");
                auto path_name=required(t,"path").string();if(path_name=="translation")out.path=AnimationPath::translation;else if(path_name=="rotation")out.path=AnimationPath::rotation;else if(path_name=="scale")out.path=AnimationPath::scale;else if(path_name=="weights")throw std::runtime_error("morph-weight animation is unsupported because morph targets are not loaded");else throw std::runtime_error("unsupported animation target path");
                auto&sm=x.samplers[out.sampler];const auto&out_accessor=c.accessors[sampler_outputs[out.sampler]];const std::size_t factor=sm.interpolation==Interpolation::cubic_spline?3:1;
                if(out.path==AnimationPath::rotation&&(sm.components!=4||out_accessor.type!="VEC4"))throw std::runtime_error("rotation animation output must be FLOAT VEC4");
                if((out.path==AnimationPath::translation||out.path==AnimationPath::scale)&&(sm.components!=3||out_accessor.type!="VEC3"))throw std::runtime_error("translation/scale animation output must be FLOAT VEC3");
                if(sm.values.size()!=sm.times.size()*sm.components*factor)throw std::runtime_error("animation sampler input/output counts do not match");
                x.channels.push_back(out);
            }
            asset.animations.push_back(std::move(x));
        }
    }
    if(const auto*arr=root.get("scenes")){for(std::size_t i=0;i<arr->size();++i){const auto&s=*arr->at(i);Scene x;x.name=s.get("name")?s.get("name")->string():"scene_"+std::to_string(i);if(const auto*n=s.get("nodes")){for(std::size_t k=0;k<n->size();++k){auto id=u32(*n->at(k),"scene root node");if(id>=asset.nodes.size()||asset.nodes[id].parent!=-1)throw std::runtime_error("scene root is invalid or has a parent");x.roots.push_back(id);}}asset.scenes.push_back(std::move(x));}}if(const auto*d=root.get("scene")){asset.default_scene=u32(*d,"default scene");if(asset.default_scene>=asset.scenes.size())throw std::runtime_error("default scene index out of range");}
    return asset;
}
} // namespace

std::expected<Asset, Error> load_gltf(const std::filesystem::path& path, LoadOptions options) {
    try { return load(path, options); }
    catch (const std::exception& e) { return std::unexpected(Error{path, e.what()}); }
}

} // namespace astra::assets
