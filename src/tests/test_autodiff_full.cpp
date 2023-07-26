#include <fstream>
#include <luisa/luisa-compute.h>
#include <luisa/ir/ast2ir.h>
#include <luisa/ir/ir2ast.h>
#include <luisa/core/stl/vector.h>
#include <luisa/runtime/rhi/resource.h>
#include <random>
// #include <luisa/backends/commmon/string_sratch.h>

using namespace luisa;
using namespace luisa::compute;

struct AdCheckOptions {
    uint32_t repeats = 1024 * 1024;
    float rel_tol = 5e-2f;
    float fd_eps = 1e-3f;
    float max_precent_bad = 0.01f;
    float min_value = -1.0f;
    float max_value = 1.0f;
};
LUISA_STRUCT(AdCheckOptions, repeats, rel_tol, fd_eps, max_precent_bad, min_value, max_value){};
using B = Buffer<float>;
template<int N, typename F>
void test_ad_helper(luisa::string_view name, Device &device, F &&f_, AdCheckOptions options = AdCheckOptions{}) {
    auto stream = device.create_stream(StreamTag::GRAPHICS);
    auto rng = std::mt19937{std::random_device{}()};
    const auto input_data = [&] {
        auto input_data = luisa::vector<luisa::vector<float>>();
        for (auto i = 0; i < N; i++) {
            auto tmp = luisa::vector<float>();
            tmp.resize(options.repeats);
            std::uniform_real_distribution<float> dist{options.min_value, options.max_value};
            for (auto j = 0; j < options.repeats; j++) {
                tmp[j] = dist(rng);
            }
            input_data.emplace_back(std::move(tmp));
        }
        return input_data;
    }();
    const auto inputs = [&] {
        auto inputs = luisa::vector<B>();
        for (auto i = 0; i < N; i++) {
            auto tmp = device.create_buffer<float>(options.repeats);
            stream << tmp.copy_from(input_data[i].data()) << synchronize();
            inputs.emplace_back(std::move(tmp));
        }
        return inputs;
    }();
    const auto dinputs_fd = [&] {
        auto dinputs_fd = luisa::vector<B>();
        for (auto i = 0; i < N; i++) {
            dinputs_fd.emplace_back(device.create_buffer<float>(options.repeats));
        }
        return dinputs_fd;
    }();
    const auto dinputs_ad = [&] {
        auto dinputs_ad = luisa::vector<B>();
        for (auto i = 0; i < N; i++) {
            dinputs_ad.emplace_back(device.create_buffer<float>(options.repeats));
        }
        return dinputs_ad;
    }();
    auto f = [&](luisa::span<Var<float>> x) {
        if constexpr (N == 1) {
            return f_(x[0]);
        } else if constexpr (N == 2) {
            return f_(x[0], x[1]);
        } else if constexpr (N == 3) {
            return f_(x[0], x[1], x[2]);
        } else if constexpr (N == 4) {
            return f_(x[0], x[1], x[2], x[3]);
        } else if constexpr (N == 5) {
            return f_(x[0], x[1], x[2], x[3], x[4]);
        } else if constexpr (N == 6) {
            return f_(x[0], x[1], x[2], x[3], x[4], x[5]);
        } else if constexpr (N == 7) {
            return f_(x[0], x[1], x[2], x[3], x[4], x[5], x[6]);
        } else if constexpr (N == 8) {
            return f_(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
        } else if constexpr (N == 9) {
            return f_(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8]);
        } else if constexpr (N == 10) {
            return f_(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8], x[9]);
        } else {
            LUISA_ERROR_WITH_LOCATION("N > 10 is not supported.");
        }
    };
    Kernel1D fd_kernel = [&](Var<AdCheckOptions> options) {
        const auto i = dispatch_x();
        auto x = luisa::vector<Var<float>>();
        for (auto j = 0; j < N; j++) {
            x.emplace_back(def(inputs[j]->read(i)));
        }
        auto eval_f = [&](int comp, Expr<float> dx) {
            auto x_copy = x;
            x_copy[comp] += dx;
            auto y = f(x_copy);
            return y;
        };
        auto dx = luisa::vector<Var<float>>();
        for (auto j = 0; j < N; j++) {
            auto f_plus_xi = eval_f(j, options.fd_eps);
            auto f_minus_xi = eval_f(j, -options.fd_eps);
            dx.emplace_back(def((f_plus_xi - f_minus_xi) / (2 * options.fd_eps)));
        }
        for (auto j = 0; j < N; j++) {
            dinputs_fd[j]->write(i, dx[j]);
        }
    };
    Kernel1D ad_kernel = [&](Var<AdCheckOptions> options) {
        const auto i = dispatch_x();
        auto x = luisa::vector<Var<float>>();
        for (auto j = 0; j < N; j++) {
            x.emplace_back(def(inputs[j]->read(i)));
        }
        $autodiff {
            for (auto j = 0; j < N; j++) {
                requires_grad(x[j]);
            }
            auto y = f(x);
            backward(y);
            for (auto j = 0; j < N; j++) {
                dinputs_ad[j]->write(i, grad(x[j]));
            }
        };
    };
    auto o = luisa::compute::ShaderOption();
    o.enable_fast_math = false;
    stream
        << device.compile(fd_kernel, o)(options).dispatch(options.repeats)
        << device.compile(ad_kernel, o)(options).dispatch(options.repeats)
        << synchronize();
    const auto fd_data = [&] {
        auto fd_data = luisa::vector<luisa::vector<float>>();
        for (auto i = 0; i < N; i++) {
            luisa::vector<float> tmp;
            tmp.resize(options.repeats);
            stream << dinputs_fd[i].copy_to(tmp.data()) << synchronize();
            fd_data.emplace_back(std::move(tmp));
        }
        return fd_data;
    }();
    const auto ad_data = [&] {
        auto ad_data = luisa::vector<luisa::vector<float>>();
        for (auto i = 0; i < N; i++) {
            luisa::vector<float> tmp;
            tmp.resize(options.repeats);
            stream << dinputs_ad[i].copy_to(tmp.data()) << synchronize();
            ad_data.emplace_back(std::move(tmp));
        }
        return ad_data;
    }();
    size_t bad_count = 0;
    luisa::string error_msg;
    for (size_t i = 0; i < options.repeats; i++) {
        for (size_t j = 0; j < N; j++) {
            const auto fd = fd_data[j][i];
            const auto ad = ad_data[j][i];
            const auto diff = std::abs(fd - ad);
            const auto rel_diff = diff / std::abs(fd);
            if (rel_diff > options.rel_tol) {
                error_msg.append(luisa::format("x[{}] = {}, fd = {}, ad = {}, diff = {}, rel_diff = {}\n", j, input_data[j][i], fd, ad, diff, rel_diff));
                bad_count++;
            }
        }
    }
    const auto bad_percent = static_cast<float>(bad_count) / (options.repeats * N);
    if (bad_percent > options.max_precent_bad) {
        LUISA_ERROR("Test `{}`:{}\nTest `{}`: Bad percent {}% is greater than max percent {}%.\n", name, error_msg, name, bad_percent * 100, options.max_precent_bad * 100);
    }
    LUISA_INFO("Test `{}` passed.", name);
}

#define TEST_AD_1(f, min, max) [&] {                        \
    auto options = AdCheckOptions{};                        \
    options.min_value = min;                                \
    options.max_value = max;                                \
    test_ad_helper<1>(                                      \
        #f, device, [&](auto x) { return f(x); }, options); \
}()

int main(int argc, char *argv[]) {

    luisa::log_level_info();

    auto context = Context{argv[0]};
    if (argc <= 1) {
        LUISA_INFO("Usage: {} <backend>. <backend>: cuda, dx, ispc, metal", argv[0]);
        exit(1);
    }
    auto device = context.create_device(argv[1]);
    TEST_AD_1(sin, -1.0, 1.0);
    TEST_AD_1(cos, -1.0, 1.0);
    TEST_AD_1(tan, -1.0, 1.0);
    TEST_AD_1(asin, -1.0, 1.0);
    TEST_AD_1(acos, -1.0, 1.0);
    TEST_AD_1(atan, -1.0, 1.0);
    TEST_AD_1(sinh, -1.0, 1.0);
    TEST_AD_1(cosh, -1.0, 1.0);
    TEST_AD_1(tanh, -1.0, 1.0);
    TEST_AD_1(asinh, -1.0, 1.0);
    TEST_AD_1(acosh, -1.0, 1.0);
    TEST_AD_1(atanh, -1.0, 1.0);
    TEST_AD_1(exp, -1.0, 1.0);
    TEST_AD_1(exp2, -1.0, 1.0);
    TEST_AD_1(log, 0.001, 10.0);
}