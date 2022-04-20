//
// Created by Mike Smith on 2021/3/13.
//

#include <ast/variable.h>
#include <core/logging.h>
#include <ast/expression.h>
#include <ast/function_builder.h>

namespace luisa::compute {

luisa::unique_ptr<Expression> Expression::create(Tag tag) noexcept {
    switch (tag) {
        // case Tag::ACCESS:
        //     return luisa::make_unique<AccessExpr>(nullptr, nullptr, nullptr);
        // case Tag::BINARY:
        //     return luisa::make_unique<BinaryExpr>(nullptr, BinaryOp::ADD, nullptr, nullptr);
        // case Tag::CALL:
        //     return luisa::make_unique<CallExpr>(nullptr, Function(), luisa::vector<const Expression *>());
        // case Tag::CAST:
        //     return luisa::make_unique<CastExpr>(nullptr, CastOp::BITWISE, nullptr);
        // case Tag::CONSTANT:
        //     return luisa::make_unique<ConstantExpr>(nullptr, ConstantData());
        // case Tag::LITERAL:
        //     return luisa::make_unique<LiteralExpr>
        // case Tag::MEMBER:
        //     return luisa::make_unique<MemberExpr>(nullptr, nullptr, 0);
        // case Tag::REF:
        //     return luisa::make_unique<RefExpr>(Variable());
        // case Tag::UNARY:
        //     return luisa::make_unique<UnaryExpr>(nullptr, UnaryOp::BIT_NOT, nullptr);
            
        default:// TODO
            LUISA_ERROR_WITH_LOCATION("Not implemented.");
    }
    return nullptr;
}

void RefExpr::_mark(Usage usage) const noexcept {
    detail::FunctionBuilder::current()->mark_variable_usage(
        _variable.uid(), usage);
}

void CallExpr::_mark() const noexcept {
    if (is_builtin()) {
        if (_op == CallOp::BUFFER_WRITE ||
            _op == CallOp::TEXTURE_WRITE ||
            _op == CallOp::SET_INSTANCE_TRANSFORM ||
            _op == CallOp::SET_INSTANCE_VISIBILITY ||
            _op == CallOp::ATOMIC_EXCHANGE ||
            _op == CallOp::ATOMIC_COMPARE_EXCHANGE ||
            _op == CallOp::ATOMIC_FETCH_ADD ||
            _op == CallOp::ATOMIC_FETCH_SUB ||
            _op == CallOp::ATOMIC_FETCH_AND ||
            _op == CallOp::ATOMIC_FETCH_OR ||
            _op == CallOp::ATOMIC_FETCH_XOR ||
            _op == CallOp::ATOMIC_FETCH_MIN ||
            _op == CallOp::ATOMIC_FETCH_MAX) {
            _arguments[0]->mark(Usage::WRITE);
            for (auto i = 1u; i < _arguments.size(); i++) {
                _arguments[i]->mark(Usage::READ);
            }
        } else {
            for (auto arg : _arguments) {
                arg->mark(Usage::READ);
            }
        }
    } else {
        auto args = _custom.arguments();
        for (auto i = 0u; i < args.size(); i++) {
            auto arg = args[i];
            _arguments[i]->mark(
                arg.tag() == Variable::Tag::REFERENCE ||
                        arg.tag() == Variable::Tag::BUFFER ||
                        arg.tag() == Variable::Tag::ACCEL ||
                        arg.tag() == Variable::Tag::TEXTURE ?
                    _custom.variable_usage(arg.uid()) :
                    Usage::READ);
        }
    }
}

void Expression::mark(Usage usage) const noexcept {
    if (auto a = to_underlying(_usage), u = a | to_underlying(usage); a != u) {
        _usage = static_cast<Usage>(u);
        _mark(usage);
    }
}

uint64_t Expression::hash() const noexcept {
    if (!_hash_computed) {
        using namespace std::string_view_literals;
        _hash = hash64(_tag, hash64(_compute_hash(), hash64("__hash_expression")));
        if (_type != nullptr) { _hash = hash64(_type->hash(), _hash); }
        _hash_computed = true;
    }
    return _hash;
}

}// namespace luisa::compute
