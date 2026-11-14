// Granny type-system walker: reads any object described by a granny_data_type_definition array into a Value tree.
#include <cmath>
#include <cstring>

#include "gr2.h"

namespace gr2 {

const Value* Value::get(const std::string& name) const {
    for (auto& kv : m)
        if (kv.first == name) return &kv.second;
    return nullptr;
}

static float halfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    float v;
    if (exp == 0)
        v = std::ldexp((float)man, -24);
    else if (exp == 31)
        v = man ? NAN : INFINITY;
    else
        v = std::ldexp((float)(man | 0x400), (int)exp - 25);
    return sign ? -v : v;
}

// Entry size in the type definition array (Type, Name*, ReferenceType*, ArrayWidth, Extra[3], Ignored) = 32 bytes.
static const uint32_t kDefSize = 32;

const TypeDef& Walker::type(uint32_t off) {
    auto it = cache_.find(off);
    if (it != cache_.end()) return it->second;
    TypeDef& td = cache_[off];
    td.offset = off;
    for (uint32_t i = 0; i < 4096; i++) {
        uint32_t p = off + i * kDefSize;
        if (!f_.valid(p, kDefSize)) break;
        TypeMember m;
        m.type = f_.i32(p);
        if (m.type == kEnd) break;
        m.name = f_.cstr(f_.u32(p + 4));
        m.refType = f_.u32(p + 8);
        m.arrayWidth = f_.i32(p + 12);
        for (int k = 0; k < 3; k++) m.extra[k] = f_.i32(p + 16 + 4 * k);
        td.members.push_back(m);
    }
    uint32_t sz = 0;
    for (auto& m : td.members) sz += memberSize(m);
    td.size = sz;
    for (auto& m : td.members) {
        td.key += m.name + ":" + std::to_string(m.type);
        td.key += ",";
    }
    return td;
}

uint32_t Walker::memberSize(const TypeMember& m) {
    uint32_t w = m.arrayWidth > 0 ? (uint32_t)m.arrayWidth : 1;
    switch (m.type) {
        case kInline: return m.refType ? type(m.refType).size * w : 0;
        case kReference: return 4;
        case kReferenceToArray: return 8;
        case kArrayOfReferences: return 8;
        case kVariantReference: return 8;
        case kReferenceToVariantArray: return 12;
        case kString: return 4;
        case kTransform: return 68;
        case kReal32: case kInt32: case kUInt32: return 4 * w;
        case kInt8: case kUInt8: case kBinormalInt8: case kNormalUInt8: return w;
        case kInt16: case kUInt16: case kBinormalInt16: case kNormalUInt16: case kReal16: return 2 * w;
        case kEmptyReference: return 4;
        default: return 0;
    }
}

namespace {
struct Budget {
    long nodes = 0;
};
Budget g_budget;
}  // namespace

Value Walker::read(uint32_t typeOff, uint32_t objOff, int depth) {
    Value v;
    v.kind = Value::Struct;
    if (depth > 20 || !typeOff || !f_.valid(objOff, 1)) {
        v.kind = Value::Null;
        return v;
    }
    if (depth == 0) g_budget.nodes = 0;
    if (++g_budget.nodes > 4000000) {
        v.kind = Value::Null;
        return v;
    }
    const TypeDef& td = type(typeOff);
    // copy members: type() may grow the cache (std::map references stay valid, but be explicit)
    uint32_t p = objOff;
    for (const TypeMember& mem : td.members) {
        Value mv;
        uint32_t w = mem.arrayWidth > 0 ? (uint32_t)mem.arrayWidth : 1;
        mv.memberType = mem.type;
        auto scalar = [&](int type, uint32_t at) {
            Value x;
            x.memberType = type;
            switch (type) {
                case kReal32: x.kind = Value::Real; x.r = f_.f32(at); break;
                case kReal16: x.kind = Value::Real; x.r = halfToFloat(f_.u16(at)); break;
                case kInt8: x.kind = Value::Int; x.i = (int8_t)f_.u8(at); break;
                case kUInt8: x.kind = Value::Int; x.i = f_.u8(at); break;
                case kBinormalInt8: x.kind = Value::Real; x.r = (int8_t)f_.u8(at) / 127.0; break;
                case kNormalUInt8: x.kind = Value::Real; x.r = f_.u8(at) / 255.0; break;
                case kInt16: x.kind = Value::Int; x.i = (int16_t)f_.u16(at); break;
                case kUInt16: x.kind = Value::Int; x.i = f_.u16(at); break;
                case kBinormalInt16: x.kind = Value::Real; x.r = (int16_t)f_.u16(at) / 32767.0; break;
                case kNormalUInt16: x.kind = Value::Real; x.r = f_.u16(at) / 65535.0; break;
                case kInt32: x.kind = Value::Int; x.i = f_.i32(at); break;
                case kUInt32: x.kind = Value::Int; x.i = f_.u32(at); break;
            }
            return x;
        };
        auto scalarSize = [](int type) -> uint32_t {
            switch (type) {
                case kReal32: case kInt32: case kUInt32: return 4;
                case kInt8: case kUInt8: case kBinormalInt8: case kNormalUInt8: return 1;
                default: return 2;
            }
        };
        switch (mem.type) {
            case kInline: {
                uint32_t sz = mem.refType ? type(mem.refType).size : 0;
                if (w > 1) {
                    mv.kind = Value::Array;
                    for (uint32_t k = 0; k < w; k++) mv.a.push_back(read(mem.refType, p + k * sz, depth + 1));
                } else
                    mv = read(mem.refType, p, depth + 1);
                break;
            }
            case kReference: {
                uint32_t ptr = f_.u32(p);
                if (ptr)
                    mv = read(mem.refType, ptr, depth + 1);
                else
                    mv.kind = Value::Null;
                break;
            }
            case kReferenceToArray: {
                uint32_t n = f_.u32(p), ptr = f_.u32(p + 4);
                mv.kind = Value::Array;
                if (ptr && n < 50000000 && mem.refType) {
                    const TypeDef& et = type(mem.refType);
                    uint32_t sz = et.size;
                    bool simple = et.members.size() == 1 && et.members[0].type >= kReal32 &&
                                  et.members[0].type <= kReal16 && et.members[0].arrayWidth <= 1;
                    for (uint32_t k = 0; k < n && f_.valid(ptr + k * sz); k++) {
                        if (simple)
                            mv.a.push_back(scalar(et.members[0].type, ptr + k * sz));  // flatten {Real32} wrappers
                        else
                            mv.a.push_back(read(mem.refType, ptr + k * sz, depth + 1));
                    }
                }
                break;
            }
            case kArrayOfReferences: {
                uint32_t n = f_.u32(p), ptr = f_.u32(p + 4);
                mv.kind = Value::Array;
                if (ptr && n < 50000000) {
                    for (uint32_t k = 0; k < n; k++) {
                        uint32_t q = f_.u32(ptr + 4 * k);
                        if (q)
                            mv.a.push_back(read(mem.refType, q, depth + 1));
                        else
                            mv.a.push_back(Value());
                    }
                }
                break;
            }
            case kVariantReference: {
                uint32_t t = f_.u32(p), ptr = f_.u32(p + 4);
                if (t && ptr) {
                    mv = read(t, ptr, depth + 1);
                    Value wrap;
                    wrap.kind = Value::Variant;
                    wrap.memberType = mem.type;
                    wrap.m.push_back({"value", mv});
                    mv = wrap;
                } else
                    mv.kind = Value::Null;
                break;
            }
            case kReferenceToVariantArray: {
                uint32_t t = f_.u32(p), n = f_.u32(p + 4), ptr = f_.u32(p + 8);
                mv.kind = Value::Array;
                if (t && ptr && n < 50000000) {
                    uint32_t sz = type(t).size;
                    for (uint32_t k = 0; k < n && f_.valid(ptr + k * sz); k++)
                        mv.a.push_back(read(t, ptr + k * sz, depth + 1));
                }
                break;
            }
            case kString:
                mv.kind = Value::String;
                mv.s = f_.cstr(f_.u32(p));
                break;
            case kTransform: {
                mv.kind = Value::Transform;
                mv.a.reserve(17);
                Value fl;
                fl.kind = Value::Int;
                fl.i = f_.u32(p);
                mv.a.push_back(fl);
                for (int k = 0; k < 16; k++) {
                    Value x;
                    x.kind = Value::Real;
                    x.r = f_.f32(p + 4 + 4 * k);
                    mv.a.push_back(x);
                }
                break;
            }
            case kEmptyReference:
                mv.kind = Value::Null;
                break;
            default: {
                if (mem.type >= kReal32 && mem.type <= kReal16 && mem.type != kUnsupported) {
                    uint32_t sz = scalarSize(mem.type);
                    if (w > 1) {
                        mv.kind = Value::Array;
                        for (uint32_t k = 0; k < w; k++) mv.a.push_back(scalar(mem.type, p + k * sz));
                    } else
                        mv = scalar(mem.type, p);
                }
                break;
            }
        }
        v.m.push_back({mem.name, std::move(mv)});
        p += memberSize(mem);
    }
    return v;
}

}  // namespace gr2
