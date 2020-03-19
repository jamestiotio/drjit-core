/*
    enoki/llvm.h -- Simple C++ array class with operator overloading (LLVM)

    This library implements convenient wrapper class around the C API in
    'enoki/jit.h'.

    Copyright (c) 2020 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#pragma once

#include <enoki/jit.h>
#include <enoki/traits.h>
#include <cstring>
#include <cstdio>

template <typename Value_>
struct LLVMArray {
    using Value = Value_;
    static constexpr VarType Type = var_type<Value>::value;

    LLVMArray() = default;

    ~LLVMArray() { jitc_var_ext_ref_dec(m_index); }

    LLVMArray(const LLVMArray &a) : m_index(a.m_index) {
        jitc_var_ext_ref_inc(m_index);
    }

    LLVMArray(LLVMArray &&a) : m_index(a.m_index) {
        a.m_index = 0;
    }

    template <typename T> LLVMArray(const LLVMArray<T> &v) {
        static_assert(!std::is_same<T, Value>::value,
                      "Conversion constructor called with arguments that don't "
                      "correspond to a conversion!");

        constexpr bool Signed =
            std::is_signed<T>::value && std::is_signed<Value>::value;

        const char *op;
        if (std::is_floating_point<Value>::value && std::is_integral<T>::value) {
            op = std::is_signed<T>::value ? "$r0 = sitofp <$w x $t1> $r1 to <$w x $t0>"
                                          : "$r0 = uitofp <$w x $t1> $r1 to <$w x $t0>";
        } else if (std::is_integral<Value>::value && std::is_floating_point<T>::value) {
            op = std::is_signed<Value>::value ? "$r0 = fptosi <$w x $t1> $r1 to <$w x $t0>"
                                              : "$r0 = fptoui <$w x $t1> $r1 to <$w x $t0>";
        } else if (std::is_floating_point<T>::value && std::is_floating_point<Value>::value) {
            op = sizeof(T) > sizeof(Value) ? "$r0 = fptrunc <$w x $t1> $r1 to <$w x $t0>"
                                           : "$r0 = fpext <$w x $t1> $r1 to <$w x $t0>";
        } else if (std::is_integral<T>::value && std::is_integral<Value>::value) {
            if (sizeof(T) == sizeof(Value)) {
                m_index = v.index();
                jitc_var_ext_ref_inc(m_index);
                return;
            } else {
                op = sizeof(T) > sizeof(Value)
                         ? "$r0 = trunc <$w x $t1> $r1 to <$w x $t0>"
                         : (Signed ? "$r0 = sext <$w x $t1> $r1 to <$w x $t0>"
                                   : "$r0 = zext <$w x $t1> $r1 to <$w x $t0>");
            }
        }
        else {
            jitc_fail("Unsupported conversion!");
        }

        m_index = jitc_trace_append_1(Type, op, 1, v.index());
    }

    LLVMArray(Value value) {
        uint_with_size_t<Value> value_uint;
        unsigned long long value_ull;

        if (Type == VarType::Float32) {
            double d = (double) value;
            memcpy(&value_ull, &d, sizeof(double));
        }  else {
            memcpy(&value_uint, &value, sizeof(Value));
            value_ull = (unsigned long long) value_uint;
        }

        char value_str[256];
        snprintf(value_str, 256,
            (Type == VarType::Float32 || Type == VarType::Float64) ?
            "$r0_t = insertelement <$w x $t0> undef, $t0 0x%llx, i32 0$n"
            "$r0 = shufflevector <$w x $t0> $r0_t, <$w x $t0> undef, <$w x i32> zeroinitializer" :
            "$r0_t = insertelement <$w x $t0> undef, $t0 %llu, i32 0$n"
            "$r0 = shufflevector <$w x $t0> $r0_t, <$w x $t0> undef, <$w x i32> zeroinitializer",
            value_ull);

        m_index = jitc_trace_append_0(Type, value_str, 0);
    }

    template <typename... Args, enable_if_t<(sizeof...(Args) > 1)> = 0>
    LLVMArray(Args&&... args) {
        Value data[] = { (Value) args... };
        m_index = jitc_var_copy(Type, data, sizeof...(Args));
    }

    LLVMArray &operator=(const LLVMArray &a) {
        jitc_var_ext_ref_inc(a.m_index);
        jitc_var_ext_ref_dec(m_index);
        m_index = a.m_index;
        return *this;
    }

    LLVMArray &operator=(LLVMArray &&a) {
        std::swap(m_index, a.m_index);
        return *this;
    }

    LLVMArray operator+(const LLVMArray &v) const {
        const char *op = std::is_floating_point<Value>::value
            ? "$r0 = fadd <$w x $t0> $r1, $r2"
            : "$r0 = add <$w x $t0> $r1, $r2";

        return from_index(
            jitc_trace_append_2(Type, op, 1, m_index, v.m_index));
    }

    LLVMArray operator-(const LLVMArray &v) const {
        const char *op = std::is_floating_point<Value>::value
            ? "$r0 = fsub <$w x $t0> $r1, $r2"
            : "$r0 = sub <$w x $t0> $r1, $r2";

        return from_index(
            jitc_trace_append_2(Type, op, 1, m_index, v.m_index));
    }

    LLVMArray operator*(const LLVMArray &v) const {
        const char *op = std::is_floating_point<Value>::value
            ? "$r0 = fmul <$w x $t0> $r1, $r2"
            : "$r0 = mul <$w x $t0> $r1, $r2";

        return from_index(
            jitc_trace_append_2(Type, op, 1, m_index, v.m_index));
    }

    LLVMArray operator/(const LLVMArray &v) const {
        const char *op = std::is_floating_point<Value>::value
            ? "$r0 = fdiv <$w x $t0> $r1, $r2"
            : "$r0 = div <$w x $t0> $r1, $r2";

        return from_index(
            jitc_trace_append_2(Type, op, 1, m_index, v.m_index));
    }

    LLVMArray& operator+=(const LLVMArray &v) {
        return operator=(*this + v);
    }

    LLVMArray& operator-=(const LLVMArray &v) {
        return operator=(*this + v);
    }

    LLVMArray& operator*=(const LLVMArray &v) {
        return operator=(*this + v);
    }

    LLVMArray& operator/=(const LLVMArray &v) {
        return operator=(*this + v);
    }

    LLVMArray operator-() const {
        const char *op = std::is_floating_point<Value>::value
            ? "$r0 = fsub <$w x $t0> $Z0, $r1"
            : "$r0 = sub <$w x $t0> $Z0, $r1";

        return from_index(
            jitc_trace_append_1(Type, op, 1, m_index));
    }

    friend LLVMArray fmadd(const LLVMArray &a, const LLVMArray &b,
                           const LLVMArray &c) {
        if (std::is_floating_point<Value>::value) {
            const char *op = std::is_same<Value, float>::value
                ? "$r0 = call <$w x $t0> @llvm.fma.v$wf32(<$w x $t1> $r1, <$w x $t2> $r2, <$w x $t3> $r3)"
                : "$r0 = call <$w x $t0> @llvm.fma.v$wf64(<$w x $t1> $r1, <$w x $t2> $r2, <$w x $t3> $r3)";

            return LLVMArray::from_index(jitc_trace_append_3(
                Type, op, 1, a.index(), b.index(), c.index()));
        } else {
            return a*b + c;
        }
    }

    friend LLVMArray fmsub(const LLVMArray &a, const LLVMArray &b,
                           const LLVMArray &c) {
        return fmadd(a, b, -c);
    }

    friend LLVMArray fnmadd(const LLVMArray &a, const LLVMArray &b,
                            const LLVMArray &c) {
        return fmadd(-a, b, c);
    }

    friend LLVMArray fnmsub(const LLVMArray &a, const LLVMArray &b,
                            const LLVMArray &c) {
        return fmadd(-a, b, -c);
    }

    static LLVMArray empty(size_t size) {
        size_t byte_size = size * sizeof(Value);
        void *ptr = jitc_malloc(AllocType::Host, byte_size);
        return from_index(jitc_var_map(Type, ptr, size, 1));
    }

    static LLVMArray zero(size_t size) {
        if (size == 1) {
            return LLVMArray(0);
        } else {
            uint8_t value = 0;
            size_t byte_size = size * sizeof(Value);
            void *ptr = jitc_malloc(AllocType::Host, byte_size);
            jitc_fill(VarType::UInt8, ptr, byte_size, &value);
            return from_index(jitc_var_map(Type, ptr, size, 1));
        }
    }

    static LLVMArray full(Value value, size_t size) {
        if (size == 1) {
            return LLVMArray(value);
        } else {
            size_t byte_size = size * sizeof(Value);
            void *ptr = jitc_malloc(AllocType::Host, byte_size);
            jitc_fill(Type, ptr, size, &value);
            return from_index(jitc_var_map(Type, ptr, size, 1));
        }
    }

    static LLVMArray arange(size_t size) {
        return arange(0, (size_t) size, 1);
    }

    static LLVMArray arange(ssize_t start, ssize_t stop, ssize_t step) {
        size_t size = size_t((stop - start + step - (step > 0 ? 1 : -1)) / step);

        using UInt32 = LLVMArray<uint32_t>;
        UInt32 index = UInt32::from_index(jitc_trace_append_0(
            VarType::UInt32,
            "$r0_0 = trunc i64 $i to i32$n"
            "$r0_1 = insertelement <$w x i32> undef, i32 $r0_0, i32 0$n"
            "$r0_2 = shufflevector <$w x i32> $r0_1, <$w x i32> undef, "
            "<$w x i32> zeroinitializer$n"
            "$r0 = add <$w x i32> $r0_2, $o0",
            1));
        jitc_var_set_size(index.index(), size, false);

        if (start == 0 && step == 1)
            return index;
        else
            return fmadd(LLVMArray(index), LLVMArray((Value) step), LLVMArray((Value) start));
    }

    LLVMArray eval() const {
        jitc_var_eval(m_index);
        return *this;
    }

    bool valid() const { return m_index != 0; }

    size_t size() const {
        return jitc_var_size(m_index);
    }

    uint32_t index() const {
        return m_index;
    }

    const char *str() {
        return jitc_var_str(m_index);
    }

    const Value *data() const {
        return jitc_var_ptr(m_index);
    }

    Value *data() {
        return jitc_var_ptr(m_index);
    }

    static LLVMArray from_index(uint32_t index) {
        LLVMArray result;
        result.m_index = index;
        return result;
    }

protected:
    uint32_t m_index = 0;
};


template <typename Value> LLVMArray<Value> hsum(const LLVMArray<Value> &v) {
    using Array = LLVMArray<Value>;
    if (v.size() == 1)
        return v;

    v.eval();
    Array result = Array::empty(1);
    jitc_reduce(Array::Type, ReductionType::Add, v.data(), v.size(),
                result.data());
    return result;
}

template <typename Value> LLVMArray<Value> hprod(const LLVMArray<Value> &v) {
    using Array = LLVMArray<Value>;
    if (v.size() == 1)
        return v;

    v.eval();
    Array result = Array::empty(1);
    jitc_reduce(Array::Type, ReductionType::Mul, v.data(), v.size(),
                result.data());
    return result;
}

template <typename Value> LLVMArray<Value> hmax(const LLVMArray<Value> &v) {
    using Array = LLVMArray<Value>;
    if (v.size() == 1)
        return v;

    v.eval();
    Array result = Array::empty(1);
    jitc_reduce(Array::Type, ReductionType::Max, v.data(), v.size(),
                result.data());
    return result;
}

template <typename Value> LLVMArray<Value> hmin(const LLVMArray<Value> &v) {
    using Array = LLVMArray<Value>;
    if (v.size() == 1)
        return v;

    v.eval();
    Array result = Array::empty(1);
    jitc_reduce(Array::Type, ReductionType::Min, v.data(), v.size(),
                result.data());
    return result;
}
