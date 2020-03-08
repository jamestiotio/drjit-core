#include "jit.h"
#include "log.h"
#include "ssa.h"
#include "eval.h"

// ====================================================================
//  The following data structures are temporarily used during program
//  generation. They are declared as global variables to enable memory
//  reuse across jit_eval() calls.
// ====================================================================

/// Ordered unique list containing sizes of variables to be computed
static std::vector<uint32_t> schedule_sizes;

/// Map variable size => ordered list of variables that should be computed
static tsl::robin_map<uint32_t, std::vector<uint32_t>> schedule;

/// Auxiliary data structure needed to compute 'schedule_sizes' and 'schedule'
static tsl::robin_set<std::pair<uint32_t, uint32_t>, pair_hash> visited;

/// Maps between Enoki variable indices and program variables
static tsl::robin_map<uint32_t, uint32_t> reg_map;

/// Name of the last generated kernel
static char kernel_name[9];

// ====================================================================

static const char *cuda_register_type(uint32_t type) {
    switch (type) {
        case EnokiType::UInt8:    return "u8";
        case EnokiType::Int8:     return "s8";
        case EnokiType::UInt16:   return "u16";
        case EnokiType::Int16:    return "s16";
        case EnokiType::UInt32:   return "u32";
        case EnokiType::Int32:    return "s32";
        case EnokiType::Pointer:
        case EnokiType::UInt64:   return "u64";
        case EnokiType::Int64:    return "s64";
        case EnokiType::Float16:  return "f16";
        case EnokiType::Float32:  return "f32";
        case EnokiType::Float64:  return "f64";
        case EnokiType::Bool:     return "pred";
        default: jit_fail("cuda_register_type(): invalid type!");
    }
}

static const char *cuda_register_type_bin(uint32_t type) {
    switch (type) {
        case EnokiType::UInt8:
        case EnokiType::Int8:    return "b8";
        case EnokiType::UInt16:
        case EnokiType::Float16:
        case EnokiType::Int16:   return "b16";
        case EnokiType::Float32:
        case EnokiType::UInt32:
        case EnokiType::Int32:   return "b32";
        case EnokiType::Pointer:
        case EnokiType::Float64:
        case EnokiType::UInt64:
        case EnokiType::Int64:   return "b64";
        case EnokiType::Bool:    return "pred";
        default: jit_fail("cuda_register_type_bin(): invalid type!");
    }
}

static const char *cuda_register_name(uint32_t type) {
    switch (type) {
        case EnokiType::UInt8:
        case EnokiType::Int8:    return "%b";
        case EnokiType::UInt16:
        case EnokiType::Int16:   return "%w";
        case EnokiType::UInt32:
        case EnokiType::Int32:   return "%r";
        case EnokiType::Pointer:
        case EnokiType::UInt64:
        case EnokiType::Int64:   return "%rd";
        case EnokiType::Float32: return "%f";
        case EnokiType::Float64: return "%d";
        case EnokiType::Bool:    return "%p";
        default: jit_fail("cuda_register_name(): invalid type!");
    }
}

// ====================================================================

/// Recursively traverse the computation graph to find variables needed by a computation
static void jit_var_traverse(uint32_t size, uint32_t idx) {
    std::pair<uint32_t, uint32_t> key(size, idx);

    if (idx == 0 || visited.find(key) != visited.end())
        return;

    visited.insert(key);

    Variable *v = jit_var(idx);
    const uint32_t *dep = v->dep;

    std::pair<uint32_t, uint32_t> ch[3] = {
        { dep[0], dep[0] ? jit_var(dep[0])->tsize : 0 },
        { dep[1], dep[1] ? jit_var(dep[1])->tsize : 0 },
        { dep[2], dep[2] ? jit_var(dep[2])->tsize : 0 }
    };

    // Simple sorting network
    if (ch[1].second < ch[2].second)
        std::swap(ch[1], ch[2]);
    if (ch[0].second < ch[2].second)
        std::swap(ch[0], ch[2]);
    if (ch[0].second < ch[1].second)
        std::swap(ch[0], ch[1]);

    for (auto [id, tsize] : ch)
        jit_var_traverse(size, id);

    schedule[size].push_back(idx);
}

void jit_assemble(uint32_t size) {
    const std::vector<uint32_t> &sched = schedule[size];
    uint32_t n_vars_in = 0, n_vars_out = 0, n_vars_total = 0;

    (void) timer();
    jit_log(Trace, "jit_assemble(size=%u): register map:", size);

    for (uint32_t index : sched) {
        const Variable *v = jit_var(index);

        if (v->data || v->direct_pointer) {
            n_vars_in++;
        } else {
            if (!v->side_effect &&
                 v->ref_count_ext > 0 &&
                 v->size == size)
            n_vars_out++;
        }

        if (state.log_level >= 4) {
            buffer.clear();
            buffer.fmt("   - %s%u -> %u",
                       cuda_register_name(v->type),
                       n_vars_total, index);

            if (v->label)
                buffer.fmt(" \"%s\"", v->label);
            if (v->size == 1)
                buffer.put(" [scalar]");
            if (v->data != nullptr || v->direct_pointer)
                buffer.put(" [in]");
            else if (v->side_effect)
                buffer.put(" [se]");
            else if (v->ref_count_ext > 0 && v->size == size)
                buffer.put(" [out]");

            jit_log(Trace, "%s", buffer.get());
        }

        reg_map[index] = n_vars_total++;
    }

    jit_log(Debug, "jit_run(): launching kernel (n=%u, in=%u, out=%u, ops=%u) ..",
            size, n_vars_in, n_vars_out, n_vars_total);

    buffer.clear();
    buffer.put(".version 6.3\n");
    buffer.put(".target sm_61\n");
    buffer.put(".address_size 64\n");

    // When a kernel doesn't have too many parameters, we can pass them directly
    uint32_t n_vars_inout = n_vars_in + n_vars_out;
    bool parameter_direct = n_vars_inout < 128;

    // auto param_ref = [&](int index) {
    //     if (parameter_direct)
    //         buffer.fmt("[arg%u]", index);
    //     else
    //         buffer.fmt("[%arg + %u]", index * 8);
    // };

    if (parameter_direct) {
        buffer.put(".visible .entry enoki_@@@@@@@@(.param .u32 size,\n");
        for (uint32_t index = 0; index < n_vars_inout; ++index) {
            buffer.fmt("                               .param .u64 arg%u%s\n",
                       index, (index + 1 < n_vars_inout) ? "," : ") {");
        }
    } else {
        buffer.put(".visible .entry enoki_@@@@@@@@(.param .u32 size,\n");
        buffer.put("                               .param .u64 arg) {\n");
    }

    uint32_t n_vars_decl = std::max(3u, n_vars_total);
    buffer.fmt("    .reg.b8 %%b<%u>;\n", n_vars_decl);
    buffer.fmt("    .reg.b16 %%w<%u>;\n", n_vars_decl);
    buffer.fmt("    .reg.b32 %%r<%u>, %%size, %%index, %%step;\n", n_vars_decl);
    buffer.fmt("    .reg.b64 %%rd<%u>, %%arg;\n", n_vars_decl);
    buffer.fmt("    .reg.f32 %%f<%u>;\n", n_vars_decl);
    buffer.fmt("    .reg.f64 %%d<%u>;\n", n_vars_decl);
    buffer.fmt("    .reg.pred %%p<%u>, %%done;\n\n", n_vars_decl);
    buffer.put("    // Grid-stride loop setup\n");

    buffer.put("    ld.param.u32 %size, [size];\n");

    if (!parameter_direct)
        buffer.put("    ld.param.u64 %arg, [arg];\n");

    buffer.put("    mov.u32 %r0, %ctaid.x;\n");
    buffer.put("    mov.u32 %r1, %ntid.x;\n");
    buffer.put("    mov.u32 %r2, %tid.x;\n");
    buffer.put("    mad.lo.u32 %index, %r0, %r1, %r2;\n");
    buffer.put("    setp.ge.u32 %done, %index, %size;\n");
    buffer.put("    @%done bra L0;\n");
    buffer.put("\n");
    buffer.put("    mov.u32 %r0, %nctaid.x;\n");
    buffer.put("    mul.lo.u32 %step, %r1, %r0;\n");
    buffer.put("\n");
    buffer.put("L1:\n");
    buffer.put("    // Loop body\n");;
    buffer.put("\n");
    buffer.put("    add.u32     %index, %index, %step;\n");
    buffer.put("    setp.ge.u32 %done, %index, %size;\n");
    buffer.put("    @!%done bra L1;\n");
    buffer.put("\n");
    buffer.put("L0:\n");
    buffer.put("    ret;\n");
    buffer.put("}");

    /// Replace '@'s in 'enoki_@@@@@@@@' by MD5 hash
    snprintf(kernel_name, 9, "%08x", crc32(buffer.get(), buffer.size()));
    memcpy(strchr(buffer.get(), '@'), kernel_name, 8);

    jit_log(Debug, "%s", buffer.get());

#if 0
    oss << "    ld.param.u32 %r1, [size];" << std::endl
        << "    mov.u32 %r4, %tid.x;" << std::endl
        << "    mov.u32 %r5, %ctaid.x;" << std::endl
        << "    mov.u32 %r6, %ntid.x;" << std::endl
        << "    mad.lo.u32 %r2, %ctaid.x, %ntid.x, %tid.x;" << std::endl
        << "    setp.ge.u32 %p0, %r2, %r1;" << std::endl
        << "    @%p0 bra L0;" << std::endl
        << std::endl
        << "    mov.u32 %r7, %nctaid.x;" << std::endl
        << "    mul.lo.u32 %r3, %r6, %r7;" << std::endl
        << std::endl
        << "L1:" << std::endl
        << "    // Loop body" << std::endl;

    for (uint32_t index : sweep) {
        Variable &var = ctx[index];

        if (var.is_collected() || (var.cmd.empty() && var.data == nullptr && !var.direct_pointer))
            throw std::runtime_error(
                "CUDABackend: found invalid/expired variable " + std::to_string(index) + " in schedule! ");

        if (var.size != 1 && var.size != size)
            throw std::runtime_error(
                "CUDABackend: encountered arrays of incompatible size! (" +
                std::to_string(size) + " vs " + std::to_string(var.size) + ")");

        oss << std::endl;
        if (var.data || var.direct_pointer) {
            size_t idx = ptrs.size();
            ptrs.push_back(var.data);

            oss << std::endl
                << "    // Load register " << cuda_register_name(var.type) << reg_map[index];
            if (!var.label.empty())
                oss << ": " << var.label;
            oss << std::endl;

            if (!var.direct_pointer) {
                oss << "    " << (parameter_direct ? "ld.param.u64 %rd8, " : "ld.global.u64 %rd8, ")
                    << param_ref(idx) << ";" << std::endl;

                const char *load_instr = "ldu";
                if (var.size != 1) {
                    oss << "    mul.wide.u32 %rd9, %r2, " << cuda_register_size(var.type) << ";" << std::endl
                        << "    add.u64 %rd8, %rd8, %rd9;" << std::endl;
                    load_instr = "ld";
                }
                if (var.type != EnokiType::Bool) {
                    oss << "    " << load_instr << ".global." << cuda_register_type(var.type) << " "
                        << cuda_register_name(var.type) << reg_map[index] << ", [%rd8]"
                        << ";" << std::endl;
                } else {
                    oss << "    " << load_instr << ".global.u8 %w1, [%rd8];" << std::endl
                        << "    setp.ne.u16 " << cuda_register_name(var.type) << reg_map[index] << ", %w1, 0;";
                }
            } else {
                oss << "    " << (parameter_direct ? "ld.param.u64 " : "ldu.global.u64 ")
                    << cuda_register_name(var.type)
                    << reg_map[index] << ", " << param_ref(idx) << ";";
            }

            n_in++;
        } else {
            if (!var.label.empty())
                oss << "    // Compute register "
                    << cuda_register_name(var.type) << reg_map[index] << ": "
                    << var.label << std::endl;
            cuda_render_cmd(oss, ctx, reg_map, index);
            n_arith++;

            if (var.side_effect) {
                n_out++;
                continue;
            }

            if (var.ref_count_ext == 0)
                continue;

            if (var.size != size)
                continue;

            size_t size_in_bytes =
                cuda_var_size(index) * cuda_register_size(var.type);

            var.data = cuda_malloc(size_in_bytes);
            var.subtree_size = 1;
#if !defined(NDEBUG)
            if (ctx.log_level >= 4)
                std::cerr << "cuda_eval(): allocated variable " << index
                          << " -> " << var.data << " (" << size_in_bytes
                          << " bytes)" << std::endl;
#endif
            size_t idx = ptrs.size();
            ptrs.push_back(var.data);
            n_out++;

            oss << std::endl
                << "    // Store register " << cuda_register_name(var.type) << reg_map[index];
            if (!var.label.empty())
                oss << ": " << var.label;
            oss << std::endl
                << "    " << (parameter_direct ? "ld.param.u64 %rd8, " : "ld.global.u64 %rd8, ")
                << param_ref(idx) << ";" << std::endl;
            if (var.size != 1) {
                oss << "    mul.wide.u32 %rd9, %r2, " << cuda_register_size(var.type) << ";" << std::endl
                    << "    add.u64 %rd8, %rd8, %rd9;" << std::endl;
            }
            if (var.type != EnokiType::Bool) {
                oss << "    st.global." << cuda_register_type(var.type) << " [%rd8], "
                    << cuda_register_name(var.type) << reg_map[index] << ";"
                    << std::endl;
            } else {
                oss << "    selp.u16 %w1, 1, 0, " << cuda_register_name(var.type)
                    << reg_map[index] << ";" << std::endl;
                oss << "    st.global.u8" << " [%rd8], %w1;" << std::endl;
            }
        }
    }

    oss << std::endl
        << "    add.u32     %r2, %r2, %r3;" << std::endl
        << "    setp.ge.u32 %p0, %r2, %r1;" << std::endl
        << "    @!%p0 bra L1;" << std::endl;
#endif
}

void jit_run() {
    float codegen_time = timer();

    auto it = state.kernels.find(buffer.get());
    CUmodule cu_module = nullptr;
    CUfunction cu_kernel = nullptr;

    if (it == state.kernels.end()) {
        const uintptr_t log_size = 8192;
        std::unique_ptr<char> error_log(new char[log_size]),
                               info_log(new char[log_size]);
        CUjit_option arg[5] = {
            CU_JIT_INFO_LOG_BUFFER,
            CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES,
            CU_JIT_ERROR_LOG_BUFFER,
            CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
            CU_JIT_LOG_VERBOSE
        };

        void *argv[5] = {
            (void *) info_log.get(),
            (void *) log_size,
            (void *) error_log.get(),
            (void *) log_size,
            (void *) 1
        };

        CUlinkState link_state;
        cuda_check(cuLinkCreate(5, arg, argv, &link_state));

        int rt = cuLinkAddData(link_state, CU_JIT_INPUT_PTX, (void *) buffer.get(),
                               buffer.size(), nullptr, 0, nullptr, nullptr);
        if (rt != CUDA_SUCCESS)
            jit_fail("Assembly dump:\n\n%s\n\njit_run(): linker error:\n\n%s",
                     buffer.get(), error_log.get());

        void *link_output = nullptr;
        size_t link_output_size = 0;
        cuda_check(cuLinkComplete(link_state, &link_output, &link_output_size));
        if (rt != CUDA_SUCCESS)
            jit_fail("Assembly dump:\n\n%s\n\njit_run(): linker error:\n\n%s",
                     buffer.get(), error_log.get());

        float link_time = timer();
        bool cache_hit = strstr(info_log.get(), "ptxas info") != nullptr;
        jit_log(Debug, "Detailed linker output:\n%s", info_log.get());

        CUresult ret = cuModuleLoadData(&cu_module, link_output);
        if (ret == CUDA_ERROR_OUT_OF_MEMORY) {
            jit_malloc_trim();
            ret = cuModuleLoadData(&cu_module, link_output);
        }
        cuda_check(ret);

        // Locate the kernel entry point
        std::string name = std::string("enoki_") + kernel_name;
        cuda_check(cuModuleGetFunction(&cu_kernel, cu_module, name.c_str()));

        /// Enoki doesn't use shared memory at all..
        cuda_check(cuFuncSetAttribute(
            cu_kernel, CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES, 0));
        cuda_check(cuFuncSetAttribute(
            cu_kernel, CU_FUNC_ATTRIBUTE_PREFERRED_SHARED_MEMORY_CARVEOUT,
            CU_SHAREDMEM_CARVEOUT_MAX_L1));

        int regs;
        cuda_check(cuFuncGetAttribute(
            &regs, CU_FUNC_ATTRIBUTE_NUM_REGS, cu_kernel));

        // Destroy the linker invocation
        cuda_check(cuLinkDestroy(link_state));

        char *str = (char *) malloc(buffer.size());
        memcpy(str, buffer.get(), buffer.size() + 1);
        state.kernels[str] = { cu_module, cu_kernel };

        jit_log(Debug, "jit_run(): cache %s, codegen: %s, %s: %s, %i registers.",
                cache_hit ? "miss" : "hit",
                std::string(jit_time_string(codegen_time)).c_str(),
                cache_hit ? "link" : "load",
                std::string(jit_time_string(link_time)).c_str(),
                regs);
    } else {
        std::tie(cu_module, cu_kernel) = it.value();
        jit_log(Debug, "jit_run(): cache hit, codegen: %s.",
                jit_time_string(codegen_time));
    }
}

/// Evaluate all computation that is queued on the current device & stream
void jit_eval() {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_fail("jit_var_copy_to_device(): device and stream must be set! "
                 "(call jit_device_set() beforehand)!");

    if (stream->todo.empty())
        return;

    visited.clear();
    schedule.clear();
    schedule_sizes.clear();

    for (uint32_t idx : stream->todo) {
        size_t size = jit_var_size(idx);
        jit_var_traverse(size, idx);
        schedule_sizes.push_back(size);
    }

    stream->todo.clear();

    std::sort(schedule_sizes.begin(), schedule_sizes.end(),
              std::greater<uint32_t>());

    bool parallel_dispatch = state.parallel_dispatch && schedule.size() > 1;

    if (!parallel_dispatch) {
        jit_log(Debug, "jit_eval(): begin.");
    } else {
        jit_log(Debug, "jit_eval(): begin (parallel dispatch to %zu streams).",
                schedule.size());
        cuda_check(cudaEventRecord(stream->event, stream->handle));
    }

    uint32_t stream_idx = 1000 * stream->stream;
    for (uint32_t size : schedule_sizes) {
        jit_assemble(size);

        Stream *sub_stream = stream;
        if (parallel_dispatch) {
            jit_device_set(stream->device, stream_idx);
            sub_stream = active_stream;
            cuda_check(cudaStreamWaitEvent(sub_stream->handle, stream->event, 0));
        }

        jit_run();

        if (parallel_dispatch) {
            cuda_check(cudaEventRecord(sub_stream->event, sub_stream->handle));
            cuda_check(cudaStreamWaitEvent(stream->handle, sub_stream->event, 0));
        }

        stream_idx++;
    }

    jit_device_set(stream->device, stream->stream);

    /**
     * At this point, all variables and their dependencies are computed, which
     * means that we can remove internal edges between them. This in turn will
     * cause many of the variables to be garbage-collected.
     */
    jit_log(Debug, "jit_eval(): cleaning up..");

    for (uint32_t size : schedule_sizes) {
        const std::vector<uint32_t> &sched = schedule[size];

        for (uint32_t idx : sched) {
            auto it = state.variables.find(idx);
            if (it == state.variables.end())
                continue;

            Variable *v = &it.value();
            bool side_effect = v->side_effect;
            v->side_effect = false;
            v->dirty = false;

            if (v->data != nullptr && v->cmd != nullptr) {
                uint32_t dep[3], extra_dep = v->extra_dep;
                memcpy(dep, v->dep, sizeof(uint32_t) * 3);
                memset(v->dep, 0, sizeof(uint32_t) * 3);
                v->extra_dep = 0;
                for (int j = 0; j < 3; ++j)
                    jit_dec_ref_int(dep[j]);
                jit_dec_ref_ext(extra_dep);
            }

            if (side_effect)
                jit_dec_ref_ext(idx);
        }
    }

    jit_free_flush();
    jit_log(Debug, "jit_eval(): done.");
}
