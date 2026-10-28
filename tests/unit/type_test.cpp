// type_test.cpp — the type representation, proved.
//
// TYPES.md §2.1: invalid states do not exist. §2.3: two spellings of one type
// compare equal. §7: Error stays Error. Each section below pins one of those,
// so a failure names the rule that broke rather than a line in the
// implementation.

#include <cstdio>

#include "types/context.h"
#include "types/type.h"
#include "types/variadic.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const char* what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

// A default Type is poison, and the poison handle reports itself.
void testPoison() {
    std::printf("poison is the default, and stays poison\n");
    cider::Type fresh;
    check(fresh.isError(), "a default-constructed Type is Error");
    check(cider::errorRef().isError(), "errorRef() is Error");
    check(cider::TypeRef{nullptr}.isError(), "a null handle is Error");
    check(!(cider::TypeRef{nullptr} == cider::errorRef()),
          "null is forgotten, not poison: distinct states, both Error");
}

// Interning: equal arguments give the same pointer, different ones do not.
void testInterning() {
    std::printf("interning decides identity once\n");
    cider::TypeContext ctx;
    const cider::Type* a = ctx.basic(cider::BasicKind::I32);
    const cider::Type* b = ctx.basic(cider::BasicKind::I32);
    const cider::Type* c = ctx.basic(cider::BasicKind::U8);
    check(a == b, "two i32 spellings are the same pointer");
    check(a != c, "i32 and u8 are different pointers");
    check(cider::TypeRef{a} == cider::TypeRef{b}, "handles compare by identity");
    check(!(cider::TypeRef{a} == cider::TypeRef{c}), "different types compare unequal");
    const cider::Type* p1 = ctx.pointer(a);
    const cider::Type* p2 = ctx.pointer(a);
    const cider::Type* p3 = ctx.pointer(c);
    check(p1 == p2, "*i32 twice is one pointer");
    check(p1 != p3, "*i32 and *u8 differ");
    check(ctx.count() == 4, "four distinct types interned (poison is static, not counted)");
}

// Invalid construction is poison, never null and never a default.
void testInvalidIsPoison() {
    std::printf("invalid construction is poison\n");
    cider::TypeContext ctx;
    check(ctx.pointer(nullptr)->isError(), "a null pointee is Error");
    const cider::Type* i32 = ctx.basic(cider::BasicKind::I32);
    check(ctx.array(i32, -1)->isError(), "a negative array length is Error");
    check(ctx.array(nullptr, 3)->isError(), "a null array element is Error");
    check(ctx.slice(nullptr)->isError(), "a null slice element is Error");
    check(ctx.nominal(cider::Kind::Pointer, cider::DeclId{})->isError(),
          "a nominal of a non-nominal Kind is Error");
}

// Every factory produces the Kind it claims.
void testKinds() {
    std::printf("every factory produces its Kind\n");
    cider::TypeContext ctx;
    const cider::Type* i32 = ctx.basic(cider::BasicKind::I32);
    check(ctx.void_()->kind() == cider::Kind::Void, "void_ is Void");
    check(ctx.string()->kind() == cider::Kind::String, "string is String");
    check(i32->kind() == cider::Kind::Basic, "basic is Basic");
    check(ctx.pointer(i32)->kind() == cider::Kind::Pointer, "pointer is Pointer");
    check(ctx.array(i32, 4)->kind() == cider::Kind::Array, "array is Array");
    check(ctx.slice(i32)->kind() == cider::Kind::Slice, "slice is Slice");
    check(ctx.nominal(cider::Kind::Struct, cider::DeclId{})->kind() == cider::Kind::Struct,
          "nominal Struct is Struct");
    check(ctx.nominal(cider::Kind::Class, cider::DeclId{})->kind() == cider::Kind::Class,
          "nominal Class is Class");
    check(ctx.vaList()->kind() == cider::Kind::VaList, "vaList is VaList");
    check(ctx.vaList() == ctx.vaList(), "va_list interns to one pointer");
    cider::Signature sig({}, i32, cider::Abi::SysV);
    check(ctx.functionPtr(sig)->kind() == cider::Kind::FunctionPtr, "functionPtr is FunctionPtr");
    check(ctx.closure(sig)->kind() == cider::Kind::Closure, "closure is Closure");
    check(ctx.typeParam(cider::DeclId{}, 0)->kind() == cider::Kind::TypeParam,
          "typeParam is TypeParam");
    check(ctx.alias(cider::DeclId{})->kind() == cider::Kind::Alias, "alias is Alias");
    check(ctx.wildcard()->kind() == cider::Kind::Wildcard, "wildcard is Wildcard");
    check(ctx.functionItem()->kind() == cider::Kind::FunctionItem, "functionItem is FunctionItem");
}

// Accessors answer only for the Kind that holds the field.
void testAccessors() {
    std::printf("accessors answer only their own Kind\n");
    cider::TypeContext ctx;
    const cider::Type* i32 = ctx.basic(cider::BasicKind::I32);
    const cider::Type* p = ctx.pointer(i32);
    check(i32->basic().has_value() && *i32->basic() == cider::BasicKind::I32,
          "basic() answers Basic");
    check(!p->basic().has_value(), "basic() is empty for Pointer");
    check(p->pointee() == i32, "pointee() answers Pointer");
    check(i32->pointee()->isError(), "pointee() of Basic is poison, not null");
    const cider::Type* arr = ctx.array(i32, 5);
    check(arr->length().has_value() && *arr->length() == 5, "length() answers Array");
    check(!i32->length().has_value(), "length() is empty for Basic");
    check(arr->element() == i32, "element() answers Array");
    check(i32->element()->isError(), "element() of Basic is poison");
    check(p->signature() == nullptr, "signature() of Pointer is null");
}

// Declarations compare by module and index, nothing else.
void testDecls() {
    std::printf("declarations compare by module and index\n");
    check(cider::sameDecl(cider::DeclId{0, 3}, cider::DeclId{0, 3}), "same module and index");
    check(!cider::sameDecl(cider::DeclId{0, 3}, cider::DeclId{1, 3}), "different module");
    check(!cider::sameDecl(cider::DeclId{0, 3}, cider::DeclId{0, 4}), "different index");
}

// Signatures compare parameter-wise, plus return type and ABI.
void testSignatures() {
    std::printf("signatures compare by parameters, return and ABI\n");
    cider::TypeContext ctx;
    const cider::Type* i32 = ctx.basic(cider::BasicKind::I32);
    const cider::Type* u8 = ctx.basic(cider::BasicKind::U8);
    cider::Signature s1({{i32, false}}, i32, cider::Abi::SysV);
    cider::Signature s2({{i32, false}}, i32, cider::Abi::SysV);
    check(cider::sameSignature(s1, s2), "identical signatures");
    cider::Signature diffParam({{u8, false}}, i32, cider::Abi::SysV);
    check(!cider::sameSignature(s1, diffParam), "a different parameter type");
    cider::Signature diffMut({{i32, true}}, i32, cider::Abi::SysV);
    check(!cider::sameSignature(s1, diffMut), "mutability matters");
    cider::Signature diffAbi({{i32, false}}, i32, cider::Abi::MsAbi);
    check(!cider::sameSignature(s1, diffAbi), "ABI matters");
    cider::Signature diffRet({{i32, false}}, u8, cider::Abi::SysV);
    check(!cider::sameSignature(s1, diffRet), "return type matters");
    cider::Signature diffArity({{i32, false}, {i32, false}}, i32, cider::Abi::SysV);
    check(!cider::sameSignature(s1, diffArity), "arity matters");
}

// typeName resolves the whole identity, never a treatment name twice.
void testNames() {
    std::printf("typeName names the thing, not the treatment\n");
    cider::TypeContext ctx;
    const cider::Type* u8 = ctx.basic(cider::BasicKind::U8);
    const cider::Type* i32 = ctx.basic(cider::BasicKind::I32);
    check(cider::typeName(cider::TypeRef{u8}) == "u8", "u8 is u8");
    check(cider::typeName(cider::TypeRef{i32}) == "i32", "i32 is i32");
    check(cider::typeName(cider::TypeRef{ctx.basic(cider::BasicKind::Bool)}) == "bool",
          "bool is bool");
    check(cider::typeName(cider::TypeRef{ctx.basic(cider::BasicKind::Char)}) == "char",
          "char is char");
    check(cider::typeName(cider::TypeRef{ctx.pointer(u8)}) == "*u8", "*u8");
    check(cider::typeName(cider::TypeRef{ctx.pointer(ctx.pointer(u8))}) == "**u8", "**u8");
    check(cider::typeName(cider::TypeRef{ctx.slice(u8)}) == "[]u8", "[]u8");
    check(cider::typeName(cider::TypeRef{ctx.array(u8, 5)}) == "[u8; 5]", "[u8; 5]");
    check(cider::typeName(cider::TypeRef{ctx.void_()}) == "void", "void");
    check(cider::typeName(cider::TypeRef{ctx.string()}) == "string", "string is string");
    check(cider::typeName(cider::TypeRef{ctx.vaList()}) == "va_list", "va_list is va_list");
    check(cider::typeName(cider::TypeRef{ctx.basic(cider::BasicKind::F64)}) == "f64", "f64");
    check(cider::typeName(cider::errorRef()) == "error", "poison names itself");
    const cider::Type* t0 = ctx.typeParam(cider::DeclId{0, 3}, 0);
    const cider::Type* t1 = ctx.typeParam(cider::DeclId{1, 3}, 0);
    const cider::Type* t2 = ctx.typeParam(cider::DeclId{0, 3}, 1);
    check(cider::typeName(cider::TypeRef{t0}) != cider::typeName(cider::TypeRef{t1}),
          "the same parameter position in different modules stays distinct");
    check(cider::typeName(cider::TypeRef{t0}) != cider::typeName(cider::TypeRef{t2}),
          "two parameters of one template stay distinct");
}

// Layout: resolved for real types, reported otherwise; sizes from layout.h.
void testLayout() {
    std::printf("layout is resolved for real types, reported otherwise\n");
    cider::TypeContext ctx;
    const cider::Type* i32 = ctx.basic(cider::BasicKind::I32);
    check(i32->hasResolvedLayout(), "i32 has a layout");
    check(ctx.pointer(i32)->hasResolvedLayout(), "a pointer has a layout");
    check(!ctx.alias(cider::DeclId{})->hasResolvedLayout(), "an alias reports none");
    check(!ctx.typeParam(cider::DeclId{}, 0)->hasResolvedLayout(), "a type parameter reports none");
    check(!ctx.wildcard()->hasResolvedLayout(), "a wildcard reports none");
    check(cider::typeSize(i32) == 4, "i32 is 4");
    check(cider::typeSize(ctx.basic(cider::BasicKind::U8)) == 1, "u8 is 1");
    check(cider::typeSize(ctx.basic(cider::BasicKind::Bool)) == 1, "bool is 1");
    check(cider::typeSize(ctx.basic(cider::BasicKind::F64)) == 8, "f64 is 8");
    check(cider::typeSize(ctx.pointer(i32)) == 8, "a pointer is 8");
    check(cider::typeSize(ctx.slice(i32)) == 16, "a slice is 16");
    check(cider::typeSize(ctx.array(ctx.basic(cider::BasicKind::U8), 5)) == 5, "[u8; 5] is 5");
    check(cider::typeSize(ctx.void_()) == 0, "void occupies nothing");
    check(cider::typeSize(ctx.string()) == 16, "string is 16");
    check(cider::typeSize(ctx.vaList()) == 24, "va_list is 24");
    check(cider::alignOf(ctx.vaList()) == 8, "va_list aligns to 8");
    check(cider::typeSize(ctx.alias(cider::DeclId{})) == -1, "an alias has no size to report");
}

// VARIADICS.md §4: every row of the C default-promotion table, and the types
// that have no variadic representation in this slice.
void testVariadicPromotions() {
    std::printf("variadic promotions follow the C table\n");
    cider::TypeContext ctx;
    const cider::TypeRef i32{ctx.basic(cider::BasicKind::I32)};
    const cider::TypeRef f64{ctx.basic(cider::BasicKind::F64)};
    auto promotesTo = [&](cider::BasicKind from, cider::TypeRef want) {
        auto got = cider::promoteVariadicArgument(cider::TypeRef{ctx.basic(from)}, ctx);
        return got.has_value() && *got == want;
    };
    check(promotesTo(cider::BasicKind::Bool, i32), "bool promotes to i32");
    check(promotesTo(cider::BasicKind::Char, i32), "char promotes to i32");
    check(promotesTo(cider::BasicKind::I8, i32), "i8 promotes to i32");
    check(promotesTo(cider::BasicKind::I16, i32), "i16 promotes to i32");
    check(promotesTo(cider::BasicKind::U8, i32), "u8 promotes to i32");
    check(promotesTo(cider::BasicKind::U16, i32), "u16 promotes to i32");
    check(promotesTo(cider::BasicKind::F32, f64), "f32 promotes to f64");
    auto unchanged = [&](cider::BasicKind basic) {
        cider::TypeRef type{ctx.basic(basic)};
        auto got = cider::promoteVariadicArgument(type, ctx);
        return got.has_value() && *got == type;
    };
    check(unchanged(cider::BasicKind::I32), "i32 passes unchanged");
    check(unchanged(cider::BasicKind::U32), "u32 passes unchanged");
    check(unchanged(cider::BasicKind::I64), "i64 passes unchanged");
    check(unchanged(cider::BasicKind::U64), "u64 passes unchanged");
    check(unchanged(cider::BasicKind::F64), "f64 passes unchanged");
    cider::TypeRef pointer{ctx.pointer(ctx.basic(cider::BasicKind::U8))};
    auto gotPointer = cider::promoteVariadicArgument(pointer, ctx);
    check(gotPointer.has_value() && *gotPointer == pointer,
          "a data pointer passes unchanged");
    cider::Signature sig({}, ctx.basic(cider::BasicKind::I32), cider::Abi::SysV);
    cider::TypeRef fn{ctx.functionPtr(sig)};
    auto gotFn = cider::promoteVariadicArgument(fn, ctx);
    check(gotFn.has_value() && *gotFn == fn, "a function pointer passes unchanged");
    cider::TypeRef enumeration{ctx.nominal(cider::Kind::Enum, cider::DeclId{1, 1})};
    auto gotEnum = cider::promoteVariadicArgument(enumeration, ctx);
    check(gotEnum.has_value() && *gotEnum == i32, "an enum promotes to i32");
    auto rejected = [&](cider::TypeRef type) {
        return !cider::promoteVariadicArgument(type, ctx).has_value();
    };
    check(rejected(cider::TypeRef{ctx.string()}), "a string has no variadic form");
    check(rejected(cider::TypeRef{ctx.vaList()}), "va_list has no variadic form");
    check(rejected(cider::TypeRef{ctx.array(ctx.basic(cider::BasicKind::I32), 2)}),
          "an array has no variadic form");
    check(rejected(cider::TypeRef{
              ctx.nominal(cider::Kind::Struct, cider::DeclId{1, 2})}),
          "a struct has no variadic form");
    check(rejected(cider::errorRef()), "an error has no variadic form");
}

}  // namespace

int main() {
    std::printf("type_test — the type representation, proved\n\n");
    testPoison();
    testInterning();
    testInvalidIsPoison();
    testKinds();
    testAccessors();
    testDecls();
    testSignatures();
    testNames();
    testLayout();
    testVariadicPromotions();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
