#pragma once

#include <serialize/Common.h>
#include <serialize/IJsonDatabase.h>
#include <serialize/IJsonObject.h>
#include <serialize/serializer.h>

namespace toolhub::db {

class SimpleBinaryJson;
class ConcurrentBinaryJson;
class SimpleJsonValueArray;
class SimpleJsonValueDict;

static constexpr uint8_t DICT_TYPE = 0;
static constexpr uint8_t ARRAY_TYPE = 1;

enum class ValueType : uint8_t {
    Int,
    Float,
    String,
    ValueDict,
    ValueArray,
    GUID
};

struct SimpleJsonVariant {
    WriteJsonVariant value;
    template<typename... Args>
    SimpleJsonVariant(Args &&...args)
        : value(std::forward<Args>(args)...) {
        if constexpr (sizeof...(Args) == 1) {
            static_assert(!std::is_same_v<std::remove_cvref_t<Args>..., ReadJsonVariant>, "can not copy from read json variant");
            static_assert(!std::is_same_v<std::remove_cvref_t<Args>..., SimpleJsonVariant>, "can not copy from self");
        }
    }
    SimpleJsonVariant(SimpleJsonVariant const &v) = delete;
    SimpleJsonVariant(SimpleJsonVariant &v) = delete;// : SimpleJsonVariant((SimpleJsonVariant const&)v) {}
    SimpleJsonVariant(SimpleJsonVariant &&v) noexcept : value(std::move(v.value)) {}
    SimpleJsonVariant(SimpleJsonVariant const &&v) = delete;

    [[nodiscard]] ReadJsonVariant GetVariant() const;
    [[nodiscard]] ReadJsonVariant GetVariant_Concurrent() const;
    template<typename... Args>
    void Set(Args &&...args) {
        this->~SimpleJsonVariant();
        new (this) SimpleJsonVariant(std::forward<Args>(args)...);
    }
    template<typename A>
    SimpleJsonVariant &operator=(A &&a) {
        this->~SimpleJsonVariant();
        new (this) SimpleJsonVariant(std::forward<A>(a));
        return *this;
    }
};

template<typename T>
void PushDataToVector(T &&v, std::vector<uint8_t> &serData) {
    using TT = std::remove_cvref_t<T>;
    vstd::SerDe<TT>::Set(v, serData);
}

class SimpleJsonLoader {
public:
    static bool Check(IJsonDatabase *db, SimpleJsonVariant const &var);
    static SimpleJsonVariant DeSerialize(std::span<uint8_t const> &arr, SimpleBinaryJson *db);
    static SimpleJsonVariant DeSerialize_Concurrent(std::span<uint8_t const> &arr, ConcurrentBinaryJson *db);
    static void Serialize(SimpleJsonVariant const &v, std::vector<uint8_t> &data);
    static void Serialize_Concurrent(SimpleJsonVariant const &v, std::vector<uint8_t> &data);
};

template<typename T>
T PopValue(std::span<uint8_t const> &arr) {
    using TT = std::remove_cvref_t<T>;
    return vstd::SerDe<TT>::Get(arr);
}

}// namespace toolhub::db