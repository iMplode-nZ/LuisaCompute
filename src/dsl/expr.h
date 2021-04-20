//
// Created by Mike Smith on 2020/12/2.
//

#pragma once

#include <array>
#include <string_view>

#include <runtime/image.h>
#include <runtime/volume.h>
#include <runtime/buffer.h>
#include <ast/function_builder.h>

namespace luisa::compute {
template<typename T>
struct Var;
}

namespace luisa::compute::detail {

template<typename T>
struct Expr;

template<typename T>
class ExprBase {

public:
    using ValueType = T;

protected:
    const Expression *_expression;

public:
    explicit ExprBase(const Expression *expr) noexcept : _expression{expr} {}

    template<concepts::non_pointer U>// to prevent conversion from pointer to bool
    requires concepts::constructible<T, U>
    ExprBase(U literal) noexcept : ExprBase{FunctionBuilder::current()->literal(Type::of(literal), literal)} {}

    constexpr ExprBase(ExprBase &&) noexcept = default;
    constexpr ExprBase(const ExprBase &) noexcept = default;
    [[nodiscard]] constexpr auto expression() const noexcept { return _expression; }

#define LUISA_MAKE_EXPR_BINARY_OP(op, op_concept_name, op_tag_name)                                       \
    template<typename U>                                                                                  \
    requires concepts::op_concept_name<T, U> [[nodiscard]] auto operator op(Expr<U> rhs) const noexcept { \
        using R = std::remove_cvref_t<decltype(std::declval<T>() op std::declval<U>())>;                  \
        return Expr<R>{FunctionBuilder::current()->binary(                                                \
            Type::of<R>(),                                                                                \
            BinaryOp::op_tag_name, this->expression(), rhs.expression())};                                \
    }                                                                                                     \
    template<typename U>                                                                                  \
    [[nodiscard]] auto operator op(U &&rhs) const noexcept {                                              \
        return this->operator op(Expr{std::forward<U>(rhs)});                                             \
    }
    LUISA_MAKE_EXPR_BINARY_OP(+, operator_add, ADD)
    LUISA_MAKE_EXPR_BINARY_OP(-, operator_sub, SUB)
    LUISA_MAKE_EXPR_BINARY_OP(*, operator_mul, MUL)
    LUISA_MAKE_EXPR_BINARY_OP(/, operator_div, DIV)
    LUISA_MAKE_EXPR_BINARY_OP(%, operator_mod, MOD)
    LUISA_MAKE_EXPR_BINARY_OP(&, operator_bit_and, BIT_AND)
    LUISA_MAKE_EXPR_BINARY_OP(|, operator_bit_or, BIT_OR)
    LUISA_MAKE_EXPR_BINARY_OP(^, operator_bit_Xor, BIT_XOR)
    LUISA_MAKE_EXPR_BINARY_OP(<<, operator_shift_left, SHL)
    LUISA_MAKE_EXPR_BINARY_OP(>>, operator_shift_right, SHR)
    LUISA_MAKE_EXPR_BINARY_OP(&&, operator_and, AND)
    LUISA_MAKE_EXPR_BINARY_OP(||, operator_or, OR)
    LUISA_MAKE_EXPR_BINARY_OP(==, operator_equal, EQUAL)
    LUISA_MAKE_EXPR_BINARY_OP(!=, operator_not_equal, NOT_EQUAL)
    LUISA_MAKE_EXPR_BINARY_OP(<, operator_less, LESS)
    LUISA_MAKE_EXPR_BINARY_OP(<=, operator_less_equal, LESS_EQUAL)
    LUISA_MAKE_EXPR_BINARY_OP(>, operator_greater, GREATER)
    LUISA_MAKE_EXPR_BINARY_OP(>=, operator_greater_equal, GREATER_EQUAL)
#undef LUISA_MAKE_EXPR_BINARY_OP

    template<typename U>
    requires concepts::operator_access<T, U> [[nodiscard]] auto operator[](Expr<U> index) const noexcept {
        using R = std::remove_cvref_t<decltype(std::declval<T>()[std::declval<U>()])>;
        return Expr<R>{FunctionBuilder::current()->access(
            Type::of<R>(),
            this->expression(), index.expression())};
    }

    template<typename U>
    [[nodiscard]] auto operator[](U &&index) const noexcept { return this->operator[](Expr{std::forward<U>(index)}); }

    void operator=(const ExprBase &rhs) &noexcept {
        FunctionBuilder::current()->assign(AssignOp::ASSIGN, this->expression(), rhs.expression());
    }

    void operator=(ExprBase &&rhs) &noexcept {
        FunctionBuilder::current()->assign(AssignOp::ASSIGN, this->expression(), rhs.expression());
    }

#define LUISA_MAKE_EXPR_ASSIGN_OP(op, op_concept_name, op_tag_name)                                      \
    template<typename U>                                                                                 \
    requires concepts::op_concept_name<T, U> void operator op(Expr<U> rhs) &noexcept {                   \
        FunctionBuilder::current()->assign(AssignOp::op_tag_name, this->expression(), rhs.expression()); \
    }                                                                                                    \
    template<typename U>                                                                                 \
    void operator op(U &&rhs) &noexcept {                                                                \
        return this->operator op(Expr{std::forward<U>(rhs)});                                            \
    }
    LUISA_MAKE_EXPR_ASSIGN_OP(=, assignable, ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(+=, add_assignable, ADD_ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(-=, sub_assignable, SUB_ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(*=, mul_assignable, MUL_ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(/=, div_assignable, DIV_ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(%=, mod_assignable, MOD_ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(&=, bit_and_assignable, BIT_AND_ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(|=, bit_or_assignable, BIT_OR_ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(^=, bit_xor_assignable, BIT_XOR_ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(<<=, shift_left_assignable, SHL_ASSIGN)
    LUISA_MAKE_EXPR_ASSIGN_OP(>>=, shift_right_assignable, SHR_ASSIGN)
#undef LUISA_MAKE_EXPR_ASSIGN_OP

    // casts
    template<typename Dest>
    requires concepts::static_convertible<T, Dest> [[nodiscard]] auto cast() const noexcept {
        return Expr<Dest>{FunctionBuilder::current()->cast(Type::of<Dest>(), CastOp::STATIC, _expression)};
    }

    template<typename Dest>
    requires concepts::bitwise_convertible<T, Dest> [[nodiscard]] auto as() const noexcept {
        return Expr<Dest>{FunctionBuilder::current()->cast(Type::of<Dest>(), CastOp::BITWISE, _expression)};
    }
};

template<typename T>
struct Expr : public ExprBase<T> {
    using ExprBase<T>::ExprBase;
    Expr(Expr &&another) noexcept = default;
    Expr(const Expr &another) noexcept = default;
    void operator=(Expr &&rhs) noexcept { ExprBase<T>::operator=(rhs); }
    void operator=(const Expr &rhs) noexcept { ExprBase<T>::operator=(rhs); }
};

template<typename... T>
struct Expr<std::tuple<T...>> : public ExprBase<std::tuple<T...>> {
    using ExprBase<std::tuple<T...>>::ExprBase;
    Expr(Expr &&another) noexcept = default;
    Expr(const Expr &another) noexcept = default;
    void operator=(Expr &&rhs) noexcept { ExprBase<std::tuple<T...>>::operator=(rhs); }
    void operator=(const Expr &rhs) noexcept { ExprBase<std::tuple<T...>>::operator=(rhs); }
    template<size_t i>
    [[nodiscard]] auto member() const noexcept {
        using M = std::tuple_element_t<i, std::tuple<T...>>;
        return Expr<M>{FunctionBuilder::current()->member(
            Type::of<M>(), this->expression(), i)};
    };
};

template<typename T>
struct Expr<Vector<T, 2>> : public ExprBase<Vector<T, 2>> {
    using ExprBase<Vector<T, 2>>::ExprBase;
    Expr(Expr &&another) noexcept = default;
    Expr(const Expr &another) noexcept = default;
    void operator=(Expr &&rhs) noexcept { ExprBase<Vector<T, 2>>::operator=(rhs); }
    void operator=(const Expr &rhs) noexcept { ExprBase<Vector<T, 2>>::operator=(rhs); }
    Expr<T> x{FunctionBuilder::current()->member(Type::of<T>(), ExprBase<Vector<T, 2>>::_expression, 0)};
    Expr<T> y{FunctionBuilder::current()->member(Type::of<T>(), ExprBase<Vector<T, 2>>::_expression, 1)};
};

template<typename T>
struct Expr<Vector<T, 3>> : public ExprBase<Vector<T, 3>> {
    using ExprBase<Vector<T, 3>>::ExprBase;
    Expr(Expr &&another) noexcept = default;
    Expr(const Expr &another) noexcept = default;
    void operator=(Expr &&rhs) noexcept { ExprBase<Vector<T, 3>>::operator=(rhs); }
    void operator=(const Expr &rhs) noexcept { ExprBase<Vector<T, 3>>::operator=(rhs); }
    Expr<T> x{FunctionBuilder::current()->member(Type::of<T>(), ExprBase<Vector<T, 3>>::_expression, 0)};
    Expr<T> y{FunctionBuilder::current()->member(Type::of<T>(), ExprBase<Vector<T, 3>>::_expression, 1)};
    Expr<T> z{FunctionBuilder::current()->member(Type::of<T>(), ExprBase<Vector<T, 3>>::_expression, 2)};
};

template<typename T>
struct Expr<Vector<T, 4>> : public ExprBase<Vector<T, 4>> {
    using ExprBase<Vector<T, 4>>::ExprBase;
    Expr(Expr &&another) noexcept = default;
    Expr(const Expr &another) noexcept = default;
    void operator=(Expr &&rhs) noexcept { ExprBase<Vector<T, 4>>::operator=(rhs); }
    void operator=(const Expr &rhs) noexcept { ExprBase<Vector<T, 4>>::operator=(rhs); }
    Expr<T> x{FunctionBuilder::current()->member(Type::of<T>(), ExprBase<Vector<T, 4>>::_expression, 0)};
    Expr<T> y{FunctionBuilder::current()->member(Type::of<T>(), ExprBase<Vector<T, 4>>::_expression, 1)};
    Expr<T> z{FunctionBuilder::current()->member(Type::of<T>(), ExprBase<Vector<T, 4>>::_expression, 2)};
    Expr<T> w{FunctionBuilder::current()->member(Type::of<T>(), ExprBase<Vector<T, 4>>::_expression, 3)};
};

template<typename T>
struct Expr<Buffer<T>> {

public:
    using ValueType = Buffer<T>;

private:
    const RefExpr *_expression{nullptr};

public:
    explicit Expr(const RefExpr *expr) noexcept
        : _expression{expr} {}
    explicit Expr(BufferView<T> buffer) noexcept
        : _expression{FunctionBuilder::current()->buffer_binding(
            Type::of<Buffer<T>>(),
            buffer.handle(), buffer.offset_bytes())} {}

    Expr &operator=(Expr) = delete;

    [[nodiscard]] const Expression *expression() const noexcept { return _expression; }

    template<concepts::integral I>
    [[nodiscard]] auto operator[](Expr<I> i) const noexcept {
        return Expr<T>{FunctionBuilder::current()->access(
            Type::of<T>(), _expression, i.expression())};
    };

    template<concepts::integral I>
    [[nodiscard]] decltype(auto) operator[](I i) const noexcept {
        return this->operator[](Expr<I>{i});
    };
};

template<typename T>
struct Expr<BufferView<T>> : public Expr<Buffer<T>> {
    using Expr<Buffer<T>>::Expr;
};

template<typename T>
struct Expr<Image<T>> {

public:
    using ValueType = Image<T>;

private:
    const RefExpr *_expression{nullptr};

public:
    explicit Expr(const RefExpr *expr) noexcept
        : _expression{expr} {}
    explicit Expr(ImageView<T> image) noexcept
        : _expression{FunctionBuilder::current()->image_binding(
            Type::of<Image<T>>(), image.handle(), image.offset())} {}

    Expr &operator=(Expr) = delete;

    [[nodiscard]] const Expression *expression() const noexcept { return _expression; }

    [[nodiscard]] auto read(Expr<uint2> uv) const noexcept {
        auto expr = Expr<Vector<T, 4>>{FunctionBuilder::current()->call(
            Type::of<Vector<T, 4>>(), CallOp::TEXTURE_READ,
            {_expression, uv.expression()})};
        return Var{expr};
    };

    void write(Expr<uint2> uv, Expr<Vector<T, 4>> value) const noexcept {
        FunctionBuilder::current()->call(
            CallOp::TEXTURE_WRITE,
            {_expression, uv.expression(), value.expression()});
    }
};

template<typename T>
struct Expr<ImageView<T>> : public Expr<Image<T>> {
    using Expr<Image<T>>::Expr;
};

template<typename T>
struct Expr<Volume<T>> {

public:
    using ValueType = Volume<T>;

private:
    const RefExpr *_expression{nullptr};

public:
    explicit Expr(const RefExpr *expr) noexcept
        : _expression{expr} {}
    explicit Expr(VolumeView<T> image) noexcept
        : _expression{FunctionBuilder::current()->image_binding(
            Type::of<Volume<T>>(), image.handle(), image.offset())} {}

    Expr &operator=(Expr) = delete;

    [[nodiscard]] const Expression *expression() const noexcept { return _expression; }

    [[nodiscard]] auto read(Expr<uint3> uvw) const noexcept {
        auto expr = Expr<Vector<T, 4>>{FunctionBuilder::current()->call(
            Type::of<Vector<T, 4>>(), CallOp::TEXTURE_READ,
            {_expression, uvw.expression()})};
        return Var{expr};
    };

    void write(Expr<uint3> uvw, Expr<Vector<T, 4>> value) const noexcept {
        FunctionBuilder::current()->call(
            CallOp::TEXTURE_WRITE,
            {_expression, uvw.expression(), value.expression()});
    }
};

template<typename T>
struct Expr<VolumeView<T>> : public Expr<Volume<T>> {
    using Expr<Volume<T>>::Expr;
};

// deduction guides
template<typename T>
Expr(Expr<T>) -> Expr<T>;

template<concepts::basic T>
Expr(T) -> Expr<T>;

template<typename T>
Expr(const Buffer<T> &) -> Expr<Buffer<T>>;

template<typename T>
Expr(BufferView<T>) -> Expr<Buffer<T>>;

template<typename T>
Expr(const Image<T> &) -> Expr<Image<T>>;

template<typename T>
Expr(ImageView<T>) -> Expr<Image<T>>;

template<typename T>
Expr(const Volume<T> &) -> Expr<Volume<T>>;

template<typename T>
Expr(VolumeView<T>) -> Expr<Volume<T>>;

template<typename T>
[[nodiscard]] inline const Expression *extract_expression(T &&v) noexcept {
    Expr expr{std::forward<T>(v)};
    return expr.expression();
}

template<typename T>
struct expr_value_impl {
    using type = T;
};

template<typename T>
struct expr_value_impl<Expr<T>> {
    using type = T;
};

template<typename T>
struct expr_value_impl<Var<T>> {
    using type = T;
};

template<typename T>
using expr_value = expr_value_impl<std::remove_cvref_t<T>>;

template<typename T>
using expr_value_t = typename expr_value<T>::type;

}// namespace luisa::compute::detail

#define LUISA_MAKE_GLOBAL_EXPR_UNARY_OP(op, op_concept, op_tag)                            \
    template<luisa::concepts::op_concept T>                                                \
    [[nodiscard]] inline auto operator op(luisa::compute::detail::Expr<T> expr) noexcept { \
        using R = std::remove_cvref_t<decltype(op std::declval<T>())>;                     \
        return luisa::compute::detail::Expr<R>{                                            \
            luisa::compute::FunctionBuilder::current()->unary(                             \
                luisa::compute::Type::of<R>(),                                             \
                luisa::compute::UnaryOp::op_tag,                                           \
                expr.expression())};                                                       \
    }
LUISA_MAKE_GLOBAL_EXPR_UNARY_OP(+, operator_plus, PLUS)
LUISA_MAKE_GLOBAL_EXPR_UNARY_OP(-, operator_minus, MINUS)
LUISA_MAKE_GLOBAL_EXPR_UNARY_OP(!, operator_not, NOT)
LUISA_MAKE_GLOBAL_EXPR_UNARY_OP(~, operator_bit_not, BIT_NOT)
#undef LUISA_MAKE_GLOBAL_EXPR_UNARY_OP

#define LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(op, op_concept)                     \
    template<luisa::concepts::basic Lhs, typename Rhs>                       \
    requires luisa::concepts::op_concept<Lhs, Rhs> [[nodiscard]] inline auto \
    operator op(Lhs lhs, luisa::compute::detail::Expr<Rhs> rhs) noexcept {   \
        return luisa::compute::detail::Expr{lhs} op rhs;                     \
    }
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(+, operator_add)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(-, operator_sub)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(*, operator_mul)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(/, operator_div)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(%, operator_mod)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(&, operator_bit_and)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(|, operator_bit_or)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(^, operator_bit_Xor)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(<<, operator_shift_left)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(>>, operator_shift_right)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(&&, operator_and)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(||, operator_or)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(==, operator_equal)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(!=, operator_not_equal)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(<, operator_less)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(<=, operator_less_equal)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(>, operator_greater)
LUISA_MAKE_GLOBAL_EXPR_BINARY_OP(>=, operator_greater_equal)
#undef LUISA_MAKE_GLOBAL_EXPR_BINARY_OP
