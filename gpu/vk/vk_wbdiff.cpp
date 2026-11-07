// (rx5700xt) Write-back by changed chunks.
//
// At a submission's end the GPU's writes are copied from a pool's VRAM mirror back to guest memory (vk_memory.cpp,
// memory_end_submission): every range a program bound as written, whole. On an RX 5700 XT with a Ryzen 5 5500 (PCIe 3.0)
// that was 67 MB and ~10 ms of GPU time a frame in Kokiri: one 15.2 MB lighting buffer, written by four compute programs in
// four submissions, went over the bus four times a frame although each program changed only part of it.
//
// Here a compute shader goes over the range in 4 KB chunks instead. For each chunk it takes a 64-bit fingerprint of the
// mirror's words (two 32-bit sums of a per-word mix that is a bijection of the word for its place: one word changed always
// changes both), compares it with the fingerprint of what it last wrote back there, and only when they differ copies the
// chunk's words to guest memory and keeps the new fingerprint. Reading the mirror costs VRAM bandwidth, nearly nothing; only
// changed chunks cross the bus. What guest memory ends up holding is what the whole copy wrote, except where the GPU changed
// nothing since the last write-back (there the copy rewrote the same bytes, or put back older ones over a CPU write made in
// between: left as the CPU wrote them now).
//
// The shader is built with Sirit (no GLSL toolchain): set 0 by push descriptor, binding 0 the mirror, 1 the pool's buffer in
// guest memory, 2 the fingerprints (two words a chunk, one table a pool); push constants: the range's first and end word, and
// the first chunk. RX_VK_WB_DIFF=0: whole copies, as before.
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "vk_internal.h"
#include "vk_draw.h"   // (descriptors_disturbed)
#include <sirit/sirit.h>

namespace mkwvk {
namespace {

constexpr uint32_t kChunkWords = 1024;   // 4 KB
constexpr uint32_t kThreads = 256;       // (each thread kChunkWords / kThreads words, strided: neighbours read neighbours)

VkPipeline pipe = VK_NULL_HANDLE;
VkPipelineLayout layout = VK_NULL_HANDLE;
bool tried = false;

std::vector<uint32_t> build() {
    using namespace Sirit;
    Module m{0x00010300};
    m.AddCapability(spv::Capability::Shader);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const Id void_t = m.TypeVoid(), u32 = m.TypeUInt(32), b1 = m.TypeBool();
    const Id uvec3 = m.TypeVector(u32, 3);
    const auto c = [&](uint32_t v) { return m.Constant(u32, v); };

    // the three storage buffers: runtime arrays of words
    const Id runtime = m.TypeRuntimeArray(u32);
    m.Decorate(runtime, spv::Decoration::ArrayStride, 4U);
    const Id block = m.TypeStruct(runtime);
    m.Decorate(block, spv::Decoration::Block);
    m.MemberDecorate(block, 0U, spv::Decoration::Offset, 0U);
    const Id block_ptr = m.TypePointer(spv::StorageClass::StorageBuffer, block);
    const Id word_ptr_t = m.TypePointer(spv::StorageClass::StorageBuffer, u32);
    Id bufs[3];
    for (uint32_t i = 0; i < 3; i++) {
        bufs[i] = m.AddGlobalVariable(block_ptr, spv::StorageClass::StorageBuffer);
        m.Decorate(bufs[i], spv::Decoration::DescriptorSet, 0U);
        m.Decorate(bufs[i], spv::Decoration::Binding, i);
    }
    m.Decorate(bufs[1], spv::Decoration::Coherent);   // (guest memory: written straight through to the host)
    // push constants: lo word, hi word (end), first chunk
    const Id pc_t = m.TypeStruct(u32, u32, u32);
    m.Decorate(pc_t, spv::Decoration::Block);
    for (uint32_t i = 0; i < 3; i++) m.MemberDecorate(pc_t, i, spv::Decoration::Offset, i * 4);
    const Id pc = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::PushConstant, pc_t), spv::StorageClass::PushConstant);
    const Id pc_word_t = m.TypePointer(spv::StorageClass::PushConstant, u32);
    // the workgroup's two sums, and the chunk's fingerprint as it was (read once, before anyone writes the new one)
    const Id shared_t = m.TypeArray(u32, c(4));
    const Id shared = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Workgroup, shared_t), spv::StorageClass::Workgroup);
    const Id shared_word_t = m.TypePointer(spv::StorageClass::Workgroup, u32);
    const Id lid_v = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Input, uvec3), spv::StorageClass::Input);
    m.Decorate(lid_v, spv::Decoration::BuiltIn, spv::BuiltIn::LocalInvocationId);
    const Id wid_v = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Input, uvec3), spv::StorageClass::Input);
    m.Decorate(wid_v, spv::Decoration::BuiltIn, spv::BuiltIn::WorkgroupId);

    const Id main = m.OpFunction(void_t, spv::FunctionControlMask::MaskNone, m.TypeFunction(void_t));
    m.AddLabel();
    const auto word = [&](uint32_t buf, Id index) { return m.OpAccessChain(word_ptr_t, bufs[buf], c(0), index); };
    const auto sh = [&](uint32_t i) { return m.OpAccessChain(shared_word_t, shared, c(i)); };
    const auto barrier = [&] {
        m.OpControlBarrier(c((uint32_t)spv::Scope::Workgroup), c((uint32_t)spv::Scope::Workgroup),
                           c((uint32_t)(spv::MemorySemanticsMask::AcquireRelease | spv::MemorySemanticsMask::WorkgroupMemory)));
    };
    const auto if_then = [&](Id cond, const auto& body) {
        const Id then = m.OpLabel(), merge = m.OpLabel();
        m.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
        m.OpBranchConditional(cond, then, merge);
        m.AddLabel(then);
        body();
        m.OpBranch(merge);
        m.AddLabel(merge);
    };
    const Id lid = m.OpCompositeExtract(u32, m.OpLoad(uvec3, lid_v), 0U);
    const Id wid = m.OpCompositeExtract(u32, m.OpLoad(uvec3, wid_v), 0U);
    const Id lo = m.OpLoad(u32, m.OpAccessChain(pc_word_t, pc, c(0)));
    const Id hi = m.OpLoad(u32, m.OpAccessChain(pc_word_t, pc, c(1)));
    const Id chunk = m.OpIAdd(u32, m.OpLoad(u32, m.OpAccessChain(pc_word_t, pc, c(2))), wid);
    const Id base = m.OpIMul(u32, chunk, c(kChunkWords));
    const Id is_first = m.OpIEqual(b1, lid, c(0));
    const Id f1 = word(2, m.OpIMul(u32, chunk, c(2))), f2 = word(2, m.OpIAdd(u32, m.OpIMul(u32, chunk, c(2)), c(1)));

    if_then(is_first, [&] {
        m.OpStore(sh(0), c(0));
        m.OpStore(sh(1), c(0));
        m.OpStore(sh(2), m.OpLoad(u32, f1));
        m.OpStore(sh(3), m.OpLoad(u32, f2));
    });
    barrier();

    // this thread's words: their index, whether in the range, their value (0 outside), and the two mixes
    constexpr uint32_t kPer = kChunkWords / kThreads;
    Id index[kPer], inside[kPer], value[kPer];
    Id sum1 = c(0), sum2 = c(0);
    for (uint32_t j = 0; j < kPer; j++) {
        index[j] = m.OpIAdd(u32, base, m.OpIAdd(u32, lid, c(j * kThreads)));
        inside[j] = m.OpLogicalAnd(b1, m.OpUGreaterThanEqual(b1, index[j], lo), m.OpULessThan(b1, index[j], hi));
        // (a load only in range: the binding is the whole pool, but the range's last chunk may end inside it)
        const Id safe = m.OpSelect(u32, inside[j], index[j], lo);
        value[j] = m.OpSelect(u32, inside[j], m.OpLoad(u32, word(0, safe)), c(0));
        // mix 1: (v ^ i*K) * K2, xorshift, * K3. mix 2: (v + i*K4) * K5, xorshift, * K6. Each a bijection of v for its i.
        Id a = m.OpBitwiseXor(u32, value[j], m.OpIMul(u32, index[j], c(0x9E3779B1u)));
        a = m.OpIMul(u32, a, c(0x85EBCA77u));
        a = m.OpBitwiseXor(u32, a, m.OpShiftRightLogical(u32, a, c(13)));
        a = m.OpIMul(u32, a, c(0xC2B2AE3Du));
        Id b = m.OpIAdd(u32, value[j], m.OpIMul(u32, index[j], c(0x27D4EB2Fu)));
        b = m.OpIMul(u32, b, c(0x165667B1u));
        b = m.OpBitwiseXor(u32, b, m.OpShiftRightLogical(u32, b, c(15)));
        b = m.OpIMul(u32, b, c(0xD3A2646Cu | 1u));
        sum1 = m.OpIAdd(u32, sum1, m.OpSelect(u32, inside[j], a, c(0)));
        sum2 = m.OpIAdd(u32, sum2, m.OpSelect(u32, inside[j], b, c(0)));
    }
    const Id scope = c((uint32_t)spv::Scope::Workgroup), sem = c(0);
    m.OpAtomicIAdd(u32, sh(0), scope, sem, sum1);
    m.OpAtomicIAdd(u32, sh(1), scope, sem, sum2);
    barrier();

    const Id h1 = m.OpLoad(u32, sh(0)), h2 = m.OpLoad(u32, sh(1));
    const Id changed = m.OpLogicalOr(b1, m.OpINotEqual(b1, h1, m.OpLoad(u32, sh(2))), m.OpINotEqual(b1, h2, m.OpLoad(u32, sh(3))));
    if_then(changed, [&] {
        for (uint32_t j = 0; j < kPer; j++)
            if_then(inside[j], [&] { m.OpStore(word(1, index[j]), value[j]); });
        if_then(is_first, [&] {
            m.OpStore(f1, h1);
            m.OpStore(f2, h2);
        });
    });
    m.OpReturn();
    m.OpFunctionEnd();
    m.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main", lid_v, wid_v);
    m.AddExecutionMode(main, spv::ExecutionMode::LocalSize, kThreads, 1U, 1U);
    return m.Assemble();
}

bool ready() {
    if (tried) return pipe != VK_NULL_HANDLE;
    tried = true;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) {
        logf("vk: write-back by changed chunks off: no push descriptors");
        return false;
    }
    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t i = 0; i < 3; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 3, li.pBindings = b;
    VkDescriptorSetLayout set{};
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &layout) != VK_SUCCESS) return false;
    const std::vector<uint32_t> code = build();
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = code.size() * 4, mi.pCode = code.data();
    VkShaderModule module{};
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
    vkDestroyShaderModule(dev.device, module, nullptr);
    if (r != VK_SUCCESS) {
        pipe = VK_NULL_HANDLE;
        logf("vk: write-back by changed chunks off: its pipeline was not made (%d)", (int)r);
        return false;
    }
    logf("vk: write-back by changed chunks on (RX_VK_WB_DIFF=0: whole copies)");
    return true;
}

// a pool's fingerprints: two words a 4 KB chunk of it, in VRAM
struct Prints {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkBuffer mirror = VK_NULL_HANDLE;   // (the mirror they were taken of: another one, they start again)
    uint64_t bytes = 0;
    bool filled = false;                // (set to "unknown", all ones, in a command buffer)
};
std::mutex prints_mu;
std::unordered_map<const void*, Prints> prints;   // by pool
uint64_t prints_bytes = 0;
// (outgrown fingerprint tables: never destroyed, a submission still in flight may use one. Kilobytes: a 1 GB pool's is 2 MB)
std::vector<std::pair<VkBuffer, VmaAllocation>> prints_kept;

} // namespace

bool writeback_diff_on() {
    static const bool on = !getenv("RX_VK_WB_DIFF") || atoi(getenv("RX_VK_WB_DIFF")) != 0;
    return on;
}

bool writeback_diff(VkCommandBuffer cmd, const void* pool_key, VkBuffer mirror, VkBuffer guest, uint64_t pool_bytes,
                    uint64_t lo, uint64_t hi) {
    if (!writeback_diff_on() || !ready() || (lo & 3) || (hi & 3) || hi <= lo || pool_bytes > (16ull << 30)) return false;
    const uint64_t chunks_in_pool = (pool_bytes + kChunkWords * 4 - 1) / (kChunkWords * 4);
    std::lock_guard lk(prints_mu);
    Prints& p = prints[pool_key];
    if (p.buffer && p.bytes < chunks_in_pool * 8) {   // (a larger pool: a new table; the old one kept, in flight maybe)
        prints_kept.push_back({p.buffer, p.alloc});
        prints_bytes -= p.bytes;
        p = Prints{};
    }
    if (p.buffer && p.mirror != mirror) p.mirror = mirror, p.filled = false;   // (another mirror: start again)
    if (!p.buffer) {
        const uint64_t bytes = chunks_in_pool * 8;
        if (!create_device_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, p.buffer, p.alloc)) {
            p = Prints{};
            return false;
        }
        p.mirror = mirror, p.bytes = bytes, p.filled = false;
        prints_bytes += bytes;
    }
    if (!p.filled) {   // (all ones: "never written back", no chunk's sums are taken for it)
        vkCmdFillBuffer(cmd, p.buffer, 0, p.bytes, 0xFFFFFFFFu);
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
        p.filled = true;
    }
    const VkDescriptorBufferInfo infos[3] = {{mirror, 0, VK_WHOLE_SIZE}, {guest, 0, VK_WHOLE_SIZE}, {p.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &infos[i];
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 3, w);
    const uint64_t lo_w = lo / 4, hi_w = hi / 4;
    for (uint64_t first = lo_w / kChunkWords, last = (hi_w - 1) / kChunkWords; first <= last;) {
        const uint64_t n = std::min<uint64_t>(last - first + 1, 65535);   // (a dispatch's limit, a pool over 256 MB in pieces)
        const uint32_t push[3] = {(uint32_t)lo_w, (uint32_t)hi_w, (uint32_t)first};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(cmd, (uint32_t)n, 1, 1);
        first += n;
    }
    descriptors_disturbed();   // (the replay's own pushed sets are gone)
    static int64_t calls;
    if (++calls % 20000 == 1)
        logf("vk: write-back by changed chunks: %lld ranges so far, fingerprints of %zu pools (%llu KB)", (long long)calls,
             prints.size(), (unsigned long long)(prints_bytes >> 10));
    return true;
}

/// A pool's mirror was given back: its fingerprints say nothing of the next one (filled again at their next use; the table
/// stays, a submission in flight may still read it)
void writeback_diff_forget(const void* pool_key) {
    std::lock_guard lk(prints_mu);
    auto it = prints.find(pool_key);
    if (it == prints.end()) return;
    it->second.filled = false;
    it->second.mirror = VK_NULL_HANDLE;
}

} // namespace mkwvk
