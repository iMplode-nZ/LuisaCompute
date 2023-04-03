//
// Created by Mike on 2021/11/8.
//

#include <string_view>

#include <core/logging.h>
#include <ast/type_registry.h>
#include <ast/constant_data.h>
#include <runtime/rtx/ray.h>
#include <runtime/rtx/hit.h>
#include <dsl/rtx/ray_query.h>
#include <backends/common/string_scratch.h>
#include <backends/cuda/cuda_codegen_ast.h>

namespace luisa::compute::cuda {

class CUDACodegenAST::RayQueryLowering {

public:
    struct OutlineInfo {
        uint index;
        luisa::vector<Variable> captured_variables;
    };

private:
    CUDACodegenAST *_codegen;
    luisa::unordered_map<const RayQueryStmt *, Function> _ray_query_statements;
    luisa::unordered_map<const RayQueryStmt *, OutlineInfo> _outline_infos;

private:
    void _collect_ray_query_statements(const Expression *expr) noexcept {
        switch (expr->tag()) {
            case Expression::Tag::UNARY: {
                auto unary_expr = static_cast<const UnaryExpr *>(expr);
                _collect_ray_query_statements(unary_expr->operand());
                break;
            }
            case Expression::Tag::BINARY: {
                auto binary_expr = static_cast<const BinaryExpr *>(expr);
                _collect_ray_query_statements(binary_expr->lhs());
                _collect_ray_query_statements(binary_expr->rhs());
                break;
            }
            case Expression::Tag::MEMBER: {
                auto member_expr = static_cast<const MemberExpr *>(expr);
                _collect_ray_query_statements(member_expr->self());
                break;
            }
            case Expression::Tag::ACCESS: {
                auto access_expr = static_cast<const AccessExpr *>(expr);
                _collect_ray_query_statements(access_expr->range());
                _collect_ray_query_statements(access_expr->index());
                break;
            }
            case Expression::Tag::LITERAL: break;
            case Expression::Tag::REF: break;
            case Expression::Tag::CONSTANT: break;
            case Expression::Tag::CALL: {
                auto call_expr = static_cast<const CallExpr *>(expr);
                if (!call_expr->is_builtin()) {
                    _collect_ray_query_statements(
                        call_expr->custom(),
                        call_expr->custom().body());
                }
                for (auto arg : call_expr->arguments()) {
                    _collect_ray_query_statements(arg);
                }
                break;
            }
            case Expression::Tag::CAST: {
                auto cast_expr = static_cast<const CastExpr *>(expr);
                _collect_ray_query_statements(cast_expr->expression());
                break;
            }
            case Expression::Tag::CPUCUSTOM: break;
            case Expression::Tag::GPUCUSTOM: break;
        }
    }

    void _collect_ray_query_statements(Function f, const ScopeStmt *scope) noexcept {
        for (auto s : scope->statements()) {
            switch (s->tag()) {
                case Statement::Tag::BREAK: break;
                case Statement::Tag::CONTINUE: break;
                case Statement::Tag::RETURN: {
                    auto return_stmt = static_cast<const ReturnStmt *>(s);
                    if (return_stmt->expression() != nullptr) {
                        _collect_ray_query_statements(return_stmt->expression());
                    }
                    break;
                }
                case Statement::Tag::SCOPE:
                    _collect_ray_query_statements(
                        f, static_cast<const ScopeStmt *>(s));
                    break;
                case Statement::Tag::IF: {
                    auto if_stmt = static_cast<const IfStmt *>(s);
                    _collect_ray_query_statements(if_stmt->condition());
                    _collect_ray_query_statements(f, if_stmt->true_branch());
                    _collect_ray_query_statements(f, if_stmt->false_branch());
                    break;
                }
                case Statement::Tag::LOOP: {
                    auto loop_stmt = static_cast<const LoopStmt *>(s);
                    _collect_ray_query_statements(f, loop_stmt->body());
                    break;
                }
                case Statement::Tag::EXPR: {
                    auto expr_stmt = static_cast<const ExprStmt *>(s);
                    _collect_ray_query_statements(expr_stmt->expression());
                    break;
                }
                case Statement::Tag::SWITCH: {
                    auto switch_stmt = static_cast<const SwitchStmt *>(s);
                    _collect_ray_query_statements(switch_stmt->expression());
                    _collect_ray_query_statements(f, switch_stmt->body());
                    break;
                }
                case Statement::Tag::SWITCH_CASE: {
                    auto case_stmt = static_cast<const SwitchCaseStmt *>(s);
                    _collect_ray_query_statements(f, case_stmt->body());
                    break;
                }
                case Statement::Tag::SWITCH_DEFAULT: {
                    auto default_stmt = static_cast<const SwitchDefaultStmt *>(s);
                    _collect_ray_query_statements(f, default_stmt->body());
                    break;
                }
                case Statement::Tag::ASSIGN: {
                    auto assign_stmt = static_cast<const AssignStmt *>(s);
                    _collect_ray_query_statements(assign_stmt->lhs());
                    _collect_ray_query_statements(assign_stmt->rhs());
                    break;
                }
                case Statement::Tag::FOR: {
                    auto for_stmt = static_cast<const ForStmt *>(s);
                    _collect_ray_query_statements(for_stmt->variable());
                    _collect_ray_query_statements(for_stmt->condition());
                    _collect_ray_query_statements(for_stmt->step());
                    _collect_ray_query_statements(f, for_stmt->body());
                    break;
                }
                case Statement::Tag::COMMENT: break;
                case Statement::Tag::RAY_QUERY: {
                    auto ray_query_stmt = static_cast<const RayQueryStmt *>(s);
                    _collect_ray_query_statements(ray_query_stmt->query());
                    _ray_query_statements.emplace(ray_query_stmt, f);
                    break;
                }
            }
        }
    }

    void _glob_variables(luisa::span<luisa::unordered_set<Variable> *> variable_sets,
                         const Expression *expr) noexcept {
        switch (expr->tag()) {
            case Expression::Tag::UNARY: {
                auto e = static_cast<const UnaryExpr *>(expr);
                _glob_variables(variable_sets, e->operand());
                break;
            }
            case Expression::Tag::BINARY: {
                auto e = static_cast<const BinaryExpr *>(expr);
                _glob_variables(variable_sets, e->lhs());
                _glob_variables(variable_sets, e->rhs());
                break;
            }
            case Expression::Tag::MEMBER: {
                auto e = static_cast<const MemberExpr *>(expr);
                _glob_variables(variable_sets, e->self());
                break;
            }
            case Expression::Tag::ACCESS: {
                auto e = static_cast<const AccessExpr *>(expr);
                _glob_variables(variable_sets, e->range());
                _glob_variables(variable_sets, e->index());
                break;
            }
            case Expression::Tag::LITERAL: break;
            case Expression::Tag::REF: {
                auto e = static_cast<const RefExpr *>(expr);
                for (auto s : variable_sets) {
                    if (s != nullptr) {
                        s->emplace(e->variable());
                    }
                }
                break;
            }
            case Expression::Tag::CONSTANT: break;
            case Expression::Tag::CALL: {
                auto e = static_cast<const CallExpr *>(expr);
                //                auto args = e->arguments();
                //                if (e->op() == CallOp::RAY_QUERY_PROCEDURAL_CANDIDATE_HIT ||
                //                    e->op() == CallOp::RAY_QUERY_TRIANGLE_CANDIDATE_HIT ||
                //                    e->op() == CallOp::RAY_QUERY_COMMIT_TRIANGLE ||
                //                    e->op() == CallOp::RAY_QUERY_COMMIT_PROCEDURAL ||
                //                    e->op() == CallOp::RAY_QUERY_TERMINATE) {
                //                    // the first argument is the ray query, which
                //                    // is provided through the function parameter
                //                    args = args.subspan(1u);
                //                }
                for (auto arg : e->arguments()) {
                    _glob_variables(variable_sets, arg);
                }
                break;
            }
            case Expression::Tag::CAST: {
                auto e = static_cast<const CastExpr *>(expr);
                _glob_variables(variable_sets, e->expression());
                break;
            }
            case Expression::Tag::CPUCUSTOM: [[fallthrough]];
            case Expression::Tag::GPUCUSTOM: LUISA_ERROR_WITH_LOCATION(
                "Custom expression is not supported in CUDA backend.");
        }
    }

    void _glob_variables(luisa::unordered_set<Variable> &within_scope,
                         luisa::unordered_set<Variable> &without_scope,
                         const ScopeStmt *current, bool inside_targets,
                         luisa::span<const ScopeStmt *const> target_scopes) noexcept {
        inside_targets |= std::find(target_scopes.begin(), target_scopes.end(), current) !=
                          target_scopes.end();
        std::array sets{inside_targets ? &within_scope : nullptr,
                        inside_targets ? nullptr : &without_scope};
        for (auto s : current->statements()) {
            switch (s->tag()) {
                case Statement::Tag::BREAK: break;
                case Statement::Tag::CONTINUE: break;
                case Statement::Tag::RETURN: {
                    auto return_stmt = static_cast<const ReturnStmt *>(s);
                    if (auto expr = return_stmt->expression()) {
                        _glob_variables(sets, expr);
                    }
                    break;
                }
                case Statement::Tag::SCOPE: {
                    auto scope_stmt = static_cast<const ScopeStmt *>(s);
                    _glob_variables(within_scope, without_scope, scope_stmt, inside_targets, target_scopes);
                    break;
                }
                case Statement::Tag::IF: {
                    auto if_stmt = static_cast<const IfStmt *>(s);
                    _glob_variables(sets, if_stmt->condition());
                    _glob_variables(within_scope, without_scope, if_stmt->true_branch(), inside_targets, target_scopes);
                    _glob_variables(within_scope, without_scope, if_stmt->false_branch(), inside_targets, target_scopes);
                    break;
                }
                case Statement::Tag::LOOP: {
                    auto loop_stmt = static_cast<const LoopStmt *>(s);
                    _glob_variables(within_scope, without_scope, loop_stmt->body(), inside_targets, target_scopes);
                    break;
                }
                case Statement::Tag::EXPR: {
                    auto expr_stmt = static_cast<const ExprStmt *>(s);
                    _glob_variables(sets, expr_stmt->expression());
                    break;
                }
                case Statement::Tag::SWITCH: {
                    auto switch_stmt = static_cast<const SwitchStmt *>(s);
                    _glob_variables(sets, switch_stmt->expression());
                    _glob_variables(within_scope, without_scope, switch_stmt->body(), inside_targets, target_scopes);
                    break;
                }
                case Statement::Tag::SWITCH_CASE: {
                    auto case_stmt = static_cast<const SwitchCaseStmt *>(s);
                    _glob_variables(within_scope, without_scope, case_stmt->body(), inside_targets, target_scopes);
                    break;
                }
                case Statement::Tag::SWITCH_DEFAULT: {
                    auto default_stmt = static_cast<const SwitchDefaultStmt *>(s);
                    _glob_variables(within_scope, without_scope, default_stmt->body(), inside_targets, target_scopes);
                    break;
                }
                case Statement::Tag::ASSIGN: {
                    auto assign_stmt = static_cast<const AssignStmt *>(s);
                    _glob_variables(sets, assign_stmt->lhs());
                    _glob_variables(sets, assign_stmt->rhs());
                    break;
                }
                case Statement::Tag::FOR: {
                    auto for_stmt = static_cast<const ForStmt *>(s);
                    _glob_variables(sets, for_stmt->variable());
                    _glob_variables(sets, for_stmt->condition());
                    _glob_variables(sets, for_stmt->step());
                    _glob_variables(within_scope, without_scope, for_stmt->body(), inside_targets, target_scopes);
                    break;
                }
                case Statement::Tag::COMMENT: break;
                case Statement::Tag::RAY_QUERY: {
                    auto ray_query_stmt = static_cast<const RayQueryStmt *>(s);
                    _glob_variables(sets, ray_query_stmt->query());
                    _glob_variables(within_scope, without_scope, ray_query_stmt->on_triangle_candidate(), inside_targets, target_scopes);
                    _glob_variables(within_scope, without_scope, ray_query_stmt->on_procedural_candidate(), inside_targets, target_scopes);
                    break;
                }
            }
        }
    }

    void _create_outline_definitions(Function f, const RayQueryStmt *s) noexcept {
        // check if the ray query is already outlined
        if (_outline_infos.contains(s)) { return; }

        // sort variables used in the ray query
        std::array target_scopes{s->on_triangle_candidate(),
                                 s->on_procedural_candidate()};
        luisa::unordered_set<Variable> within_scope_variables;
        luisa::unordered_set<Variable> without_scope_variables;
        _glob_variables(within_scope_variables,
                        without_scope_variables,
                        f.body(), false,
                        target_scopes);
        // find local and captured variables
        luisa::unordered_set<Variable> local_variable_set;// V(local) = V(all) - V(function - scope)
        for (auto v : f.local_variables()) {
            if (!without_scope_variables.contains(v) &&
                !v.is_builtin() &&
                v.type() != _codegen->_ray_query_all_type &&
                v.type() != _codegen->_ray_query_any_type) {
                local_variable_set.emplace(v);
            }
        }
        luisa::vector<Variable> captured_variables;// V(captured) = V(scope) - V(local)
        for (auto v : within_scope_variables) {
            if (!local_variable_set.contains(v) &&
                !v.is_builtin() &&
                v.type() != _codegen->_ray_query_all_type &&
                v.type() != _codegen->_ray_query_any_type) {
                captured_variables.emplace_back(v);
            }
        }
        std::sort(captured_variables.begin(), captured_variables.end(), [](auto lhs, auto rhs) noexcept {
            auto lhs_size = lhs.is_resource() ? 16u : lhs.type()->alignment();
            auto rhs_size = rhs.is_resource() ? 16u : rhs.type()->alignment();
            return lhs_size > rhs_size;
        });

        // create outline struct
        auto rq_index = static_cast<uint>(_outline_infos.size());
        _codegen->_scratch << "struct LCRayQueryCtx" << rq_index << " {";
        for (auto v : captured_variables) {
            _codegen->_scratch << "\n  ";
            _codegen->_emit_variable_decl(v, false);
            _codegen->_scratch << ";";
        }
        _codegen->_scratch << "\n};\n\n";

        auto generate_intersection_body = [&](const ScopeStmt *stmt) noexcept {
            auto indent = _codegen->_indent;
            {
                _codegen->_indent = 1;
                // built-in variables
                _codegen->_emit_builtin_variables();
                _codegen->_scratch << "\n";
                // copy captured variables
                for (auto v : captured_variables) {
                    _codegen->_emit_indent();
                    _codegen->_emit_variable_decl(v, false);
                    _codegen->_scratch << " = ctx->";
                    _codegen->_emit_variable_name(v);
                    _codegen->_scratch << ";\n";
                }
                // declare local variables
                for (auto v : local_variable_set) {
                    _codegen->_emit_indent();
                    _codegen->_emit_variable_decl(v, false);
                    _codegen->_scratch << "{};\n";
                }
                // emit body
                _codegen->_emit_indent();
                _codegen->_scratch << "{ // intersection handling body\n";
                _codegen->_indent++;
                for (auto s : stmt->statements()) {
                    _codegen->_emit_indent();
                    s->accept(*_codegen);
                    _codegen->_scratch << "\n";
                }
                _codegen->_indent--;
                _codegen->_emit_indent();
                _codegen->_scratch << "} // intersection handling body\n";
                // copy back local variables
                for (auto v : captured_variables) {
                    if (!v.is_resource()) {
                        _codegen->_emit_indent();
                        _codegen->_scratch << "ctx->";
                        _codegen->_emit_variable_name(v);
                        _codegen->_scratch << " = ";
                        _codegen->_emit_variable_name(v);
                        _codegen->_scratch << ";\n";
                    }
                }
            }
            _codegen->_indent = indent;
        };

        // create outlined triangle function
        _codegen->_scratch << "LUISA_DECL_RAY_QUERY_TRIANGLE_IMPL(" << rq_index << ") {\n"
                           << "  auto ctx = static_cast<LCRayQueryCtx" << rq_index << " *>(ctx_in);\n"
                           << "  LCTriangleIntersectionResult result{};";
        generate_intersection_body(s->on_triangle_candidate());
        _codegen->_scratch << "  return result;\n"
                              "}\n\n";

        // create outlined procedural function
        _codegen->_scratch << "LUISA_DECL_RAY_QUERY_PROCEDURAL_IMPL(" << rq_index << ") {\n"
                           << "  auto ctx = static_cast<LCRayQueryCtx" << rq_index << " *>(ctx_in);\n"
                           << "  LCProceduralIntersectionResult result{};";
        generate_intersection_body(s->on_procedural_candidate());
        _codegen->_scratch << "  return result;\n"
                              "}\n\n";

        // record the outline function
        _outline_infos.emplace(s, OutlineInfo{rq_index, std::move(captured_variables)});
    }

public:
    explicit RayQueryLowering(CUDACodegenAST *codegen) noexcept
        : _codegen{codegen} {}

    void preprocess(Function f) noexcept {
        _collect_ray_query_statements(f, f.body());
        _codegen->_scratch << "#define LUISA_RAY_QUERY_IMPL_COUNT "
                           << _ray_query_statements.size() << "\n";
    }

    void outline(Function f) noexcept {
        for (auto [rq, func] : _ray_query_statements) {
            if (func == f) { _create_outline_definitions(func, rq); }
        }
    }

    void lower(const RayQueryStmt *stmt) noexcept {
        auto &&[rq_index, captured_variables] = _outline_infos.at(stmt);
        // create ray query context
        _codegen->_scratch << "\n";
        _codegen->_emit_indent();
        _codegen->_scratch << "{ // ray query #" << rq_index << "\n";
        _codegen->_indent++;
        _codegen->_emit_indent();
        _codegen->_scratch << "LCRayQueryCtx" << rq_index << " ctx{\n";
        // copy captured variables
        _codegen->_indent++;
        for (auto v : captured_variables) {
            _codegen->_emit_indent();
            // _codegen->_scratch << "  .";
            // _codegen->_emit_variable_name(v);
            // _codegen->_scratch << " = ";
            _codegen->_emit_variable_name(v);
            _codegen->_scratch << ",\n";
        }
        _codegen->_indent--;
        _codegen->_emit_indent();
        _codegen->_scratch << "};\n";
        _codegen->_emit_indent();
        _codegen->_scratch << "lc_ray_query_trace(";
        stmt->query()->accept(*_codegen);
        _codegen->_scratch << ", " << rq_index << ", &ctx);\n";
        // copy back captured variables
        for (auto v : captured_variables) {
            if (!v.is_resource()) {
                _codegen->_emit_indent();
                _codegen->_emit_variable_name(v);
                _codegen->_scratch << " = ctx.";
                _codegen->_emit_variable_name(v);
                _codegen->_scratch << ";\n";
            }
        }
        _codegen->_indent--;
        _codegen->_emit_indent();
        _codegen->_scratch << "} // ray query #" << rq_index << "\n";
    }
};

void CUDACodegenAST::visit(const UnaryExpr *expr) {
    switch (expr->op()) {
        case UnaryOp::PLUS: _scratch << "+"; break;
        case UnaryOp::MINUS: _scratch << "-"; break;
        case UnaryOp::NOT: _scratch << "!"; break;
        case UnaryOp::BIT_NOT: _scratch << "~"; break;
        default: break;
    }
    expr->operand()->accept(*this);
}

void CUDACodegenAST::visit(const BinaryExpr *expr) {
    _scratch << "(";
    expr->lhs()->accept(*this);
    switch (expr->op()) {
        case BinaryOp::ADD: _scratch << " + "; break;
        case BinaryOp::SUB: _scratch << " - "; break;
        case BinaryOp::MUL: _scratch << " * "; break;
        case BinaryOp::DIV: _scratch << " / "; break;
        case BinaryOp::MOD: _scratch << " % "; break;
        case BinaryOp::BIT_AND: _scratch << " & "; break;
        case BinaryOp::BIT_OR: _scratch << " | "; break;
        case BinaryOp::BIT_XOR: _scratch << " ^ "; break;
        case BinaryOp::SHL: _scratch << " << "; break;
        case BinaryOp::SHR: _scratch << " >> "; break;
        case BinaryOp::AND: _scratch << " && "; break;
        case BinaryOp::OR: _scratch << " || "; break;
        case BinaryOp::LESS: _scratch << " < "; break;
        case BinaryOp::GREATER: _scratch << " > "; break;
        case BinaryOp::LESS_EQUAL: _scratch << " <= "; break;
        case BinaryOp::GREATER_EQUAL: _scratch << " >= "; break;
        case BinaryOp::EQUAL: _scratch << " == "; break;
        case BinaryOp::NOT_EQUAL: _scratch << " != "; break;
    }
    expr->rhs()->accept(*this);
    _scratch << ")";
}

void CUDACodegenAST::visit(const MemberExpr *expr) {
    if (expr->is_swizzle()) {
        static constexpr std::string_view xyzw[]{"x", "y", "z", "w"};
        if (auto ss = expr->swizzle_size(); ss == 1u) {
            expr->self()->accept(*this);
            _scratch << ".";
            _scratch << xyzw[expr->swizzle_index(0)];
        } else {
            _scratch << "lc_make_";
            auto elem = expr->type()->element();
            switch (elem->tag()) {
                case Type::Tag::BOOL: _scratch << "bool"; break;
                case Type::Tag::INT32: _scratch << "int"; break;
                case Type::Tag::UINT32: _scratch << "uint"; break;
                case Type::Tag::FLOAT32: _scratch << "float"; break;
                default: LUISA_ERROR_WITH_LOCATION(
                    "Invalid vector element type: {}.",
                    elem->description());
            }
            _scratch << ss << "(";
            for (auto i = 0u; i < ss; i++) {
                expr->self()->accept(*this);
                _scratch << "." << xyzw[expr->swizzle_index(i)] << ", ";
            }
            _scratch.pop_back();
            _scratch.pop_back();
            _scratch << ")";
        }
    } else {
        expr->self()->accept(*this);
        _scratch << ".m" << expr->member_index();
    }
}

void CUDACodegenAST::visit(const AccessExpr *expr) {
    expr->range()->accept(*this);
    _scratch << "[";
    expr->index()->accept(*this);
    _scratch << "]";
}

namespace detail {

class LiteralPrinter {

private:
    StringScratch &_s;

public:
    explicit LiteralPrinter(StringScratch &s) noexcept : _s{s} {}
    void operator()(bool v) const noexcept { _s << v; }
    void operator()(float v) const noexcept {
        if (std::isnan(v)) [[unlikely]] { LUISA_ERROR_WITH_LOCATION("Encountered with NaN."); }
        if (std::isinf(v)) {
            _s << (v < 0.0f ? " __int_as_float(0xff800000)" : " __int_as_float(0x7f800000)");
        } else {
            _s << v << "f";
        }
    }
    void operator()(int v) const noexcept { _s << v; }
    void operator()(uint v) const noexcept { _s << v << "u"; }

    template<typename T, size_t N>
    void operator()(Vector<T, N> v) const noexcept {
        auto t = Type::of<T>();
        _s << "lc_make_" << t->description() << N << "(";
        for (auto i = 0u; i < N; i++) {
            (*this)(v[i]);
            _s << ", ";
        }
        _s.pop_back();
        _s.pop_back();
        _s << ")";
    }

    void operator()(float2x2 m) const noexcept {
        _s << "lc_make_float2x2(";
        for (auto col = 0u; col < 2u; col++) {
            for (auto row = 0u; row < 2u; row++) {
                (*this)(m[col][row]);
                _s << ", ";
            }
        }
        _s.pop_back();
        _s.pop_back();
        _s << ")";
    }

    void operator()(float3x3 m) const noexcept {
        _s << "lc_make_float3x3(";
        for (auto col = 0u; col < 3u; col++) {
            for (auto row = 0u; row < 3u; row++) {
                (*this)(m[col][row]);
                _s << ", ";
            }
        }
        _s.pop_back();
        _s.pop_back();
        _s << ")";
    }

    void operator()(float4x4 m) const noexcept {
        _s << "lc_make_float4x4(";
        for (auto col = 0u; col < 4u; col++) {
            for (auto row = 0u; row < 4u; row++) {
                (*this)(m[col][row]);
                _s << ", ";
            }
        }
        _s.pop_back();
        _s.pop_back();
        _s << ")";
    }
};

}// namespace detail

void CUDACodegenAST::visit(const LiteralExpr *expr) {
    luisa::visit(detail::LiteralPrinter{_scratch}, expr->value());
}

void CUDACodegenAST::visit(const RefExpr *expr) {
    _emit_variable_name(expr->variable());
}

void CUDACodegenAST::visit(const CallExpr *expr) {

    switch (expr->op()) {
        case CallOp::CUSTOM: _scratch << "custom_" << hash_to_string(expr->custom().hash()); break;
        case CallOp::ALL: _scratch << "lc_all"; break;
        case CallOp::ANY: _scratch << "lc_any"; break;
        case CallOp::SELECT: _scratch << "lc_select"; break;
        case CallOp::CLAMP: _scratch << "lc_clamp"; break;
        case CallOp::LERP: _scratch << "lc_lerp"; break;
        case CallOp::STEP: _scratch << "lc_step"; break;
        case CallOp::ABS: _scratch << "lc_abs"; break;
        case CallOp::MIN: _scratch << "lc_min"; break;
        case CallOp::MAX: _scratch << "lc_max"; break;
        case CallOp::CLZ: _scratch << "lc_clz"; break;
        case CallOp::CTZ: _scratch << "lc_ctz"; break;
        case CallOp::POPCOUNT: _scratch << "lc_popcount"; break;
        case CallOp::REVERSE: _scratch << "lc_reverse"; break;
        case CallOp::ISINF: _scratch << "lc_isinf"; break;
        case CallOp::ISNAN: _scratch << "lc_isnan"; break;
        case CallOp::ACOS: _scratch << "lc_acos"; break;
        case CallOp::ACOSH: _scratch << "lc_acosh"; break;
        case CallOp::ASIN: _scratch << "lc_asin"; break;
        case CallOp::ASINH: _scratch << "lc_asinh"; break;
        case CallOp::ATAN: _scratch << "lc_atan"; break;
        case CallOp::ATAN2: _scratch << "lc_atan2"; break;
        case CallOp::ATANH: _scratch << "lc_atanh"; break;
        case CallOp::COS: _scratch << "lc_cos"; break;
        case CallOp::COSH: _scratch << "lc_cosh"; break;
        case CallOp::SIN: _scratch << "lc_sin"; break;
        case CallOp::SINH: _scratch << "lc_sinh"; break;
        case CallOp::TAN: _scratch << "lc_tan"; break;
        case CallOp::TANH: _scratch << "lc_tanh"; break;
        case CallOp::EXP: _scratch << "lc_exp"; break;
        case CallOp::EXP2: _scratch << "lc_exp2"; break;
        case CallOp::EXP10: _scratch << "lc_exp10"; break;
        case CallOp::LOG: _scratch << "lc_log"; break;
        case CallOp::LOG2: _scratch << "lc_log2"; break;
        case CallOp::LOG10: _scratch << "lc_log10"; break;
        case CallOp::POW: _scratch << "lc_pow"; break;
        case CallOp::SQRT: _scratch << "lc_sqrt"; break;
        case CallOp::RSQRT: _scratch << "lc_rsqrt"; break;
        case CallOp::CEIL: _scratch << "lc_ceil"; break;
        case CallOp::FLOOR: _scratch << "lc_floor"; break;
        case CallOp::FRACT: _scratch << "lc_fract"; break;
        case CallOp::TRUNC: _scratch << "lc_trunc"; break;
        case CallOp::ROUND: _scratch << "lc_round"; break;
        case CallOp::FMA: _scratch << "lc_fma"; break;
        case CallOp::COPYSIGN: _scratch << "lc_copysign"; break;
        case CallOp::CROSS: _scratch << "lc_cross"; break;
        case CallOp::DOT: _scratch << "lc_dot"; break;
        case CallOp::LENGTH: _scratch << "lc_length"; break;
        case CallOp::LENGTH_SQUARED: _scratch << "lc_length_squared"; break;
        case CallOp::NORMALIZE: _scratch << "lc_normalize"; break;
        case CallOp::FACEFORWARD: _scratch << "lc_faceforward"; break;
        case CallOp::DETERMINANT: _scratch << "lc_determinant"; break;
        case CallOp::TRANSPOSE: _scratch << "lc_transpose"; break;
        case CallOp::INVERSE: _scratch << "lc_inverse"; break;
        case CallOp::SYNCHRONIZE_BLOCK: _scratch << "lc_synchronize_block"; break;
        case CallOp::ATOMIC_EXCHANGE: _scratch << "lc_atomic_exchange"; break;
        case CallOp::ATOMIC_COMPARE_EXCHANGE: _scratch << "lc_atomic_compare_exchange"; break;
        case CallOp::ATOMIC_FETCH_ADD: _scratch << "lc_atomic_fetch_add"; break;
        case CallOp::ATOMIC_FETCH_SUB: _scratch << "lc_atomic_fetch_sub"; break;
        case CallOp::ATOMIC_FETCH_AND: _scratch << "lc_atomic_fetch_and"; break;
        case CallOp::ATOMIC_FETCH_OR: _scratch << "lc_atomic_fetch_or"; break;
        case CallOp::ATOMIC_FETCH_XOR: _scratch << "lc_atomic_fetch_xor"; break;
        case CallOp::ATOMIC_FETCH_MIN: _scratch << "lc_atomic_fetch_min"; break;
        case CallOp::ATOMIC_FETCH_MAX: _scratch << "lc_atomic_fetch_max"; break;
        case CallOp::BUFFER_READ: _scratch << "lc_buffer_read"; break;
        case CallOp::BUFFER_WRITE: _scratch << "lc_buffer_write"; break;
        case CallOp::TEXTURE_READ:
            _scratch << "lc_surf"
                     << expr->arguments().front()->type()->dimension() << "d_read<"
                     << "lc_" << expr->arguments().front()->type()->element()->description() << ">";
            break;
        case CallOp::TEXTURE_WRITE:
            _scratch << "lc_surf"
                     << expr->arguments().front()->type()->dimension() << "d_write<"
                     << "lc_" << expr->arguments().front()->type()->element()->description() << ">";
            break;
        case CallOp::BINDLESS_TEXTURE2D_SAMPLE: _scratch << "lc_bindless_texture_sample2d"; break;
        case CallOp::BINDLESS_TEXTURE2D_SAMPLE_LEVEL: _scratch << "lc_bindless_texture_sample2d_level"; break;
        case CallOp::BINDLESS_TEXTURE2D_SAMPLE_GRAD: _scratch << "lc_bindless_texture_sample2d_grad"; break;
        case CallOp::BINDLESS_TEXTURE3D_SAMPLE: _scratch << "lc_bindless_texture_sample3d"; break;
        case CallOp::BINDLESS_TEXTURE3D_SAMPLE_LEVEL: _scratch << "lc_bindless_texture_sample3d_level"; break;
        case CallOp::BINDLESS_TEXTURE3D_SAMPLE_GRAD: _scratch << "lc_bindless_texture_sample3d_grad"; break;
        case CallOp::BINDLESS_TEXTURE2D_READ: _scratch << "lc_bindless_texture_read2d"; break;
        case CallOp::BINDLESS_TEXTURE3D_READ: _scratch << "lc_bindless_texture_read3d"; break;
        case CallOp::BINDLESS_TEXTURE2D_READ_LEVEL: _scratch << "lc_bindless_texture_read2d_level"; break;
        case CallOp::BINDLESS_TEXTURE3D_READ_LEVEL: _scratch << "lc_bindless_texture_read3d_level"; break;
        case CallOp::BINDLESS_TEXTURE2D_SIZE: _scratch << "lc_bindless_texture_size2d"; break;
        case CallOp::BINDLESS_TEXTURE3D_SIZE: _scratch << "lc_bindless_texture_size3d"; break;
        case CallOp::BINDLESS_TEXTURE2D_SIZE_LEVEL: _scratch << "lc_bindless_texture_size2d_level"; break;
        case CallOp::BINDLESS_TEXTURE3D_SIZE_LEVEL: _scratch << "lc_bindless_texture_size3d_level"; break;
        case CallOp::BINDLESS_BUFFER_READ:
            _scratch << "lc_bindless_buffer_read<";
            _emit_type_name(expr->type());
            _scratch << ">";
            break;
#define LUISA_CUDA_CODEGEN_MAKE_VECTOR_CALL(type, tag)                      \
    case CallOp::MAKE_##tag##2: _scratch << "lc_make_" << #type "2"; break; \
    case CallOp::MAKE_##tag##3: _scratch << "lc_make_" << #type "3"; break; \
    case CallOp::MAKE_##tag##4: _scratch << "lc_make_" << #type "4"; break;
            LUISA_CUDA_CODEGEN_MAKE_VECTOR_CALL(bool, BOOL)
            LUISA_CUDA_CODEGEN_MAKE_VECTOR_CALL(int, INT)
            LUISA_CUDA_CODEGEN_MAKE_VECTOR_CALL(uint, UINT)
            LUISA_CUDA_CODEGEN_MAKE_VECTOR_CALL(float, FLOAT)
#undef LUISA_CUDA_CODEGEN_MAKE_VECTOR_CALL
        case CallOp::MAKE_FLOAT2X2: _scratch << "lc_make_float2x2"; break;
        case CallOp::MAKE_FLOAT3X3: _scratch << "lc_make_float3x3"; break;
        case CallOp::MAKE_FLOAT4X4: _scratch << "lc_make_float4x4"; break;
        case CallOp::ASSUME: _scratch << "__builtin_assume"; break;
        case CallOp::UNREACHABLE: _scratch << "__builtin_unreachable"; break;
        case CallOp::RAY_TRACING_INSTANCE_TRANSFORM: _scratch << "lc_accel_instance_transform"; break;
        case CallOp::RAY_TRACING_SET_INSTANCE_TRANSFORM: _scratch << "lc_accel_set_instance_transform"; break;
        case CallOp::RAY_TRACING_SET_INSTANCE_VISIBILITY: _scratch << "lc_accel_set_instance_visibility"; break;
        case CallOp::RAY_TRACING_SET_INSTANCE_OPACITY: _scratch << "lc_accel_set_instance_opacity"; break;
        case CallOp::RAY_TRACING_TRACE_CLOSEST: _scratch << "lc_accel_trace_closest"; break;
        case CallOp::RAY_TRACING_TRACE_ANY: _scratch << "lc_accel_trace_any"; break;
        case CallOp::RAY_TRACING_QUERY_ALL: _scratch << "lc_accel_query_all"; break;
        case CallOp::RAY_TRACING_QUERY_ANY: _scratch << "lc_accel_query_any"; break;
        case CallOp::RAY_QUERY_PROCEDURAL_CANDIDATE_HIT: _scratch << "LC_RAY_QUERY_PROCEDURAL_CANDIDATE_HIT"; break;
        case CallOp::RAY_QUERY_TRIANGLE_CANDIDATE_HIT: _scratch << "LC_RAY_QUERY_TRIANGLE_CANDIDATE_HIT"; break;
        case CallOp::RAY_QUERY_COMMITTED_HIT: _scratch << "lc_ray_query_committed_hit"; break;
        case CallOp::RAY_QUERY_COMMIT_TRIANGLE: _scratch << "LC_RAY_QUERY_COMMIT_TRIANGLE"; break;
        case CallOp::RAY_QUERY_COMMIT_PROCEDURAL: _scratch << "LC_RAY_QUERY_COMMIT_PROCEDURAL"; break;
        case CallOp::RAY_QUERY_TERMINATE: _scratch << "LC_RAY_QUERY_TERMINATE"; break;
    }
    _scratch << "(";
    if (auto args = expr->arguments(); !args.empty()) {
        for (auto arg : args) {
            arg->accept(*this);
            _scratch << ", ";
        }
        _scratch.pop_back();
        _scratch.pop_back();
    }
    _scratch << ")";
}

void CUDACodegenAST::visit(const CastExpr *expr) {
    switch (expr->op()) {
        case CastOp::STATIC:
            _scratch << "static_cast<";
            _emit_type_name(expr->type());
            _scratch << ">(";
            break;
        case CastOp::BITWISE:
            _scratch << "lc_bit_cast<";
            _emit_type_name(expr->type());
            _scratch << ">(";
            break;
        default: break;
    }
    expr->expression()->accept(*this);
    _scratch << ")";
}

void CUDACodegenAST::visit(const BreakStmt *) {
    _scratch << "break;";
}

void CUDACodegenAST::visit(const ContinueStmt *) {
    _scratch << "continue;";
}

void CUDACodegenAST::visit(const ReturnStmt *stmt) {
    _scratch << "return";
    if (auto expr = stmt->expression(); expr != nullptr) {
        _scratch << " ";
        expr->accept(*this);
    }
    _scratch << ";";
}

void CUDACodegenAST::visit(const ScopeStmt *stmt) {
    _scratch << "{";
    _emit_statements(stmt->statements());
    _scratch << "}";
}

void CUDACodegenAST::visit(const IfStmt *stmt) {
    _scratch << "if (";
    stmt->condition()->accept(*this);
    _scratch << ") ";
    stmt->true_branch()->accept(*this);
    if (auto fb = stmt->false_branch(); fb != nullptr && !fb->statements().empty()) {
        _scratch << " else ";
        if (auto elif = dynamic_cast<const IfStmt *>(fb->statements().front());
            fb->statements().size() == 1u && elif != nullptr) {
            elif->accept(*this);
        } else {
            fb->accept(*this);
        }
    }
}

void CUDACodegenAST::visit(const LoopStmt *stmt) {
    _scratch << "for (;;) ";
    stmt->body()->accept(*this);
}

void CUDACodegenAST::visit(const ExprStmt *stmt) {
    stmt->expression()->accept(*this);
    _scratch << ";";
}

void CUDACodegenAST::visit(const SwitchStmt *stmt) {
    _scratch << "switch (";
    stmt->expression()->accept(*this);
    _scratch << ") ";
    stmt->body()->accept(*this);
}

void CUDACodegenAST::visit(const SwitchCaseStmt *stmt) {
    _scratch << "case ";
    stmt->expression()->accept(*this);
    _scratch << ": ";
    stmt->body()->accept(*this);
}

void CUDACodegenAST::visit(const SwitchDefaultStmt *stmt) {
    _scratch << "default: ";
    stmt->body()->accept(*this);
}

void CUDACodegenAST::visit(const AssignStmt *stmt) {
    stmt->lhs()->accept(*this);
    _scratch << " = ";
    stmt->rhs()->accept(*this);
    _scratch << ";";
}

void CUDACodegenAST::visit(const RayQueryStmt *stmt) {
    _ray_query_lowering->lower(stmt);
}

void CUDACodegenAST::emit(Function f) {
    if (f.requires_raytracing()) {
        _scratch << "#define LUISA_ENABLE_OPTIX\n";
        if (f.propagated_builtin_callables().test(CallOp::RAY_TRACING_TRACE_CLOSEST)) {
            _scratch << "#define LUISA_ENABLE_OPTIX_TRACE_CLOSEST\n";
        }
        if (f.propagated_builtin_callables().test(CallOp::RAY_TRACING_TRACE_ANY)) {
            _scratch << "#define LUISA_ENABLE_OPTIX_TRACE_ANY\n";
        }
        if (f.propagated_builtin_callables().test(CallOp::RAY_TRACING_QUERY_ALL) ||
            f.propagated_builtin_callables().test(CallOp::RAY_TRACING_QUERY_ANY)) {
            _scratch << "#define LUISA_ENABLE_OPTIX_RAY_QUERY\n";
            _ray_query_lowering->preprocess(f);
        }
    }
    _scratch << "#define LC_BLOCK_SIZE lc_make_uint3("
             << f.block_size().x << ", "
             << f.block_size().y << ", "
             << f.block_size().z << ")\n\n"
             << "#include \"device_library.h\"\n\n";
    _emit_type_decl();
    _emit_function(f);
}

void CUDACodegenAST::_emit_function(Function f) noexcept {

    if (auto iter = std::find(_generated_functions.cbegin(), _generated_functions.cend(), f);
        iter != _generated_functions.cend()) { return; }
    _generated_functions.emplace_back(f);

    for (auto &&callable : f.custom_callables()) { _emit_function(callable->function()); }

    _indent = 0u;
    _function = f;

    // constants
    if (!f.constants().empty()) {
        for (auto c : f.constants()) { _emit_constant(c); }
        _scratch << "\n";
    }

    // ray tracing kernels use __constant__ args
    if (f.tag() == Function::Tag::KERNEL && f.requires_raytracing()) {
        _scratch << "struct alignas(16) Params {";
        for (auto arg : f.arguments()) {
            _scratch << "\n  alignas(16) ";
            _emit_variable_decl(arg, !arg.type()->is_buffer());
            _scratch << "{};";
        }
        _scratch << "\n};\n\nextern \"C\" "
                    "{ __constant__ Params params; }\n\n";
    }

    // outline ray query functions
    if (f.direct_builtin_callables().test(CallOp::RAY_TRACING_QUERY_ALL) ||
        f.direct_builtin_callables().test(CallOp::RAY_TRACING_QUERY_ANY)) {
        _ray_query_lowering->outline(f);
    }

    // signature
    if (f.tag() == Function::Tag::KERNEL) {
        _scratch << "extern \"C\" __global__ void "
                 << (f.requires_raytracing() ?
                         "__raygen__main" :
                         "kernel_main");
    } else if (f.tag() == Function::Tag::CALLABLE) {
        _scratch << "inline __device__ ";
        if (f.return_type() != nullptr) {
            _emit_type_name(f.return_type());
        } else {
            _scratch << "void";
        }
        _scratch << " custom_" << hash_to_string(f.hash());
    } else [[unlikely]] {
        LUISA_ERROR_WITH_LOCATION("Invalid function type.");
    }
    _scratch << "(";
    if (f.tag() == Function::Tag::KERNEL && f.requires_raytracing()) {
        _scratch << ") {";
        for (auto arg : f.arguments()) {
            _scratch << "\n  ";
            if (auto usage = f.variable_usage(arg.uid());
                usage == Usage::WRITE || usage == Usage::READ_WRITE) {
                _scratch << "auto ";
            } else {
                _scratch << "const auto &";
            }
            _emit_variable_name(arg);
            _scratch << " = params.";
            _emit_variable_name(arg);
            _scratch << ";";
        }
    } else {
        auto any_arg = false;
        for (auto arg : f.arguments()) {
            _scratch << "\n    ";
            _emit_variable_decl(arg, false);
            _scratch << ",";
            any_arg = true;
        }
        if (f.tag() == Function::Tag::KERNEL) {
            _scratch << "\n"
                     << "    const lc_uint3 dispatch_size) {";
        } else {
            if (any_arg) { _scratch.pop_back(); }
            _scratch << ") noexcept {";
        }
    }
    // emit built-in variables
    if (f.tag() == Function::Tag::KERNEL) {
        _emit_builtin_variables();
        if (!f.requires_raytracing()) {
            _scratch << "\n  if (lc_any(did >= dispatch_size)) { return; }";
        }
    }
    _indent = 1;
    _emit_variable_declarations(f);
    _indent = 0;
    _emit_statements(f.body()->statements());
    _scratch << "}\n\n";
}

void CUDACodegenAST::_emit_builtin_variables() noexcept {
    _scratch
        // block size
        << "\n  constexpr auto bs = lc_block_size();"
        // launch size
        << "\n  const auto ls = lc_dispatch_size();"
        // dispatch id
        << "\n  const auto did = lc_dispatch_id();"
        // thread id
        << "\n  const auto tid = lc_thread_id();"
        // block id
        << "\n  const auto bid = lc_block_id();";
}

void CUDACodegenAST::_emit_variable_name(Variable v) noexcept {
    switch (v.tag()) {
        case Variable::Tag::LOCAL: _scratch << "v" << v.uid(); break;
        case Variable::Tag::SHARED: _scratch << "s" << v.uid(); break;
        case Variable::Tag::REFERENCE: _scratch << "r" << v.uid(); break;
        case Variable::Tag::BUFFER: _scratch << "b" << v.uid(); break;
        case Variable::Tag::TEXTURE: _scratch << "i" << v.uid(); break;
        case Variable::Tag::BINDLESS_ARRAY: _scratch << "h" << v.uid(); break;
        case Variable::Tag::ACCEL: _scratch << "a" << v.uid(); break;
        case Variable::Tag::THREAD_ID: _scratch << "tid"; break;
        case Variable::Tag::BLOCK_ID: _scratch << "bid"; break;
        case Variable::Tag::DISPATCH_ID: _scratch << "did"; break;
        case Variable::Tag::DISPATCH_SIZE: _scratch << "ls"; break;
    }
}

void CUDACodegenAST::_emit_type_decl() noexcept {
    Type::traverse(*this);
}

void CUDACodegenAST::visit(const Type *type) noexcept {
    if (type->is_structure() &&
        type != _ray_type &&
        type != _triangle_hit_type &&
        type != _procedural_hit_type &&
        type != _committed_hit_type &&
        type != _ray_query_all_type &&
        type != _ray_query_any_type) {
        _scratch << "struct alignas(" << type->alignment() << ") ";
        _emit_type_name(type);
        _scratch << " {\n";
        for (auto i = 0u; i < type->members().size(); i++) {
            _scratch << "  ";
            _emit_type_name(type->members()[i]);
            _scratch << " m" << i << "{};\n";
        }
        _scratch << "};\n\n";
    }
}

void CUDACodegenAST::_emit_type_name(const Type *type) noexcept {

    switch (type->tag()) {
        case Type::Tag::BOOL: _scratch << "lc_bool"; break;
        case Type::Tag::FLOAT32: _scratch << "lc_float"; break;
        case Type::Tag::INT32: _scratch << "lc_int"; break;
        case Type::Tag::UINT32: _scratch << "lc_uint"; break;
        case Type::Tag::VECTOR:
            _emit_type_name(type->element());
            _scratch << type->dimension();
            break;
        case Type::Tag::MATRIX:
            _scratch << "lc_float"
                     << type->dimension()
                     << "x"
                     << type->dimension();
            break;
        case Type::Tag::ARRAY:
            _scratch << "lc_array<";
            _emit_type_name(type->element());
            _scratch << ", ";
            _scratch << type->dimension() << ">";
            break;
        case Type::Tag::STRUCTURE: {
            if (type == _ray_type) {
                _scratch << "LCRay";
            } else if (type == _triangle_hit_type) {
                _scratch << "LCTriangleHit";
            } else if (type == _procedural_hit_type) {
                _scratch << "LCProceduralHit";
            } else if (type == _committed_hit_type) {
                _scratch << "LCCommittedHit";
            } else {
                _scratch << "S" << hash_to_string(type->hash());
            }
            break;
        }
        case Type::Tag::CUSTOM: {
            if (type == _ray_query_all_type) {
                _scratch << "LCRayQueryAll";
            } else if (type == _ray_query_any_type) {
                _scratch << "LCRayQueryAny";
            } else {
                LUISA_ERROR_WITH_LOCATION(
                    "Unsupported custom type: {}.",
                    type->description());
            }
            break;
        }
        default: break;
    }
}

void CUDACodegenAST::_emit_variable_decl(Variable v, bool force_const) noexcept {
    auto usage = _function.variable_usage(v.uid());
    auto readonly = usage == Usage::NONE || usage == Usage::READ;
    switch (v.tag()) {
        case Variable::Tag::SHARED:
            _scratch << "__shared__ ";
            _emit_type_name(v.type());
            _scratch << " ";
            _emit_variable_name(v);
            break;
        case Variable::Tag::REFERENCE:
            if (readonly || force_const) {
                _scratch << "const ";
                _emit_type_name(v.type());
                _scratch << " ";
            } else {
                _emit_type_name(v.type());
                _scratch << " &";
            }
            _emit_variable_name(v);
            break;
        case Variable::Tag::BUFFER:
            _scratch << "const LCBuffer<";
            if (readonly || force_const) { _scratch << "const "; }
            _emit_type_name(v.type()->element());
            _scratch << "> ";
            _emit_variable_name(v);
            break;
        case Variable::Tag::TEXTURE:
            _scratch << "const LCSurface ";
            _emit_variable_name(v);
            break;
        case Variable::Tag::BINDLESS_ARRAY:
            _scratch << "const LCBindlessArray ";
            _emit_variable_name(v);
            break;
        case Variable::Tag::ACCEL:
            _scratch << "const LCAccel ";
            _emit_variable_name(v);
            break;
        default:
            _emit_type_name(v.type());
            _scratch << " ";
            _emit_variable_name(v);
            break;
    }
}

void CUDACodegenAST::_emit_indent() noexcept {
    for (auto i = 0u; i < _indent; i++) { _scratch << "  "; }
}

void CUDACodegenAST::_emit_statements(luisa::span<const Statement *const> stmts) noexcept {
    _indent++;
    for (auto s : stmts) {
        _scratch << "\n";
        _emit_indent();
        s->accept(*this);
    }
    _indent--;
    if (!stmts.empty()) {
        _scratch << "\n";
        _emit_indent();
    }
}

void CUDACodegenAST::_emit_constant(Function::Constant c) noexcept {

    if (std::find(_generated_constants.cbegin(),
                  _generated_constants.cend(), c.data.hash()) != _generated_constants.cend()) { return; }
    _generated_constants.emplace_back(c.data.hash());

    _scratch << "__constant__ LC_CONSTANT ";
    _emit_type_name(c.type);
    _scratch << " c" << hash_to_string(c.data.hash()) << "{";
    auto count = c.type->dimension();
    static constexpr auto wrap = 16u;
    using namespace std::string_view_literals;
    luisa::visit(
        [count, this](auto ptr) {
            detail::LiteralPrinter print{_scratch};
            for (auto i = 0u; i < count; i++) {
                if (count > wrap && i % wrap == 0u) { _scratch << "\n    "; }
                print(ptr[i]);
                _scratch << ", ";
            }
        },
        c.data.view());
    if (count > 0u) {
        _scratch.pop_back();
        _scratch.pop_back();
    }
    _scratch << "};\n";
}

void CUDACodegenAST::visit(const ConstantExpr *expr) {
    _scratch << "c" << hash_to_string(expr->data().hash());
}

void CUDACodegenAST::visit(const ForStmt *stmt) {
    _scratch << "for (; ";
    stmt->condition()->accept(*this);
    _scratch << "; ";
    stmt->variable()->accept(*this);
    _scratch << " += ";
    stmt->step()->accept(*this);
    _scratch << ") ";
    stmt->body()->accept(*this);
}

void CUDACodegenAST::visit(const CommentStmt *stmt) {
    _scratch << "/* " << stmt->comment() << " */";
}

void CUDACodegenAST::_emit_variable_declarations(Function f) noexcept {
    for (auto v : f.shared_variables()) {
        if (_function.variable_usage(v.uid()) != Usage::NONE) {
            _scratch << "\n";
            _emit_indent();
            _emit_variable_decl(v, false);
            _scratch << ";";
        }
    }
    for (auto v : f.local_variables()) {
        if (_function.variable_usage(v.uid()) != Usage::NONE) {
            _scratch << "\n";
            _emit_indent();
            _emit_variable_decl(v, false);
            _scratch << "{};";
        }
    }
}

void CUDACodegenAST::visit(const CpuCustomOpExpr *expr) {
    LUISA_ERROR_WITH_LOCATION(
        "CudaCodegen: CpuCustomOpExpr is not supported in CUDA backend.");
}

void CUDACodegenAST::visit(const GpuCustomOpExpr *expr) {
    LUISA_ERROR_WITH_LOCATION(
        "CudaCodegen: GpuCustomOpExpr is not supported in CUDA backend.");
}

CUDACodegenAST::CUDACodegenAST(StringScratch &scratch) noexcept
    : _scratch{scratch},
      _ray_type{Type::of<Ray>()},
      _triangle_hit_type{Type::of<TriangleHit>()},
      _procedural_hit_type{Type::of<ProceduralHit>()},
      _committed_hit_type{Type::of<CommittedHit>()},
      _ray_query_all_type{Type::of<RayQueryAll>()},
      _ray_query_any_type{Type::of<RayQueryAny>()},
      _ray_query_lowering{luisa::make_unique<RayQueryLowering>(this)} {}

CUDACodegenAST::~CUDACodegenAST() noexcept = default;

}// namespace luisa::compute::cuda
