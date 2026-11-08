// GPU profile (vk_gprof.h): a timeline of timestamps per recording slot.
// (rx5700xt) It timed only the passes and dispatches: on an RX 5700 XT they added up to 5.7 ms of the 17.5 ms a frame kept the
// GPU busy. Now every recording is a run of marks (its start, each span's begin and end, its end), and the time between one span's
// end and the next one's begin is counted too, under the names of what was recorded there (gprof_event: barriers, mirror syncs,
// write-backs ...): the GPU's time between the passes, and what it went to.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "vk_gprof.h"

namespace mkwvk {
namespace {

constexpr int kSlots = 48;                // (vk_replay.cpp's kRecordings)
constexpr uint32_t kMarks = 2048;         // timestamps a recording can take
VkQueryPool pool = VK_NULL_HANDLE;
struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::vector<std::string> labels;      // labels[i]: what ran between mark i - 1 and mark i (labels[0]: none)
    std::string pending;                  // (what gprof_event named since the last mark)
    bool open = false;
    bool used = false;
};
Slot slots[kSlots];
int cur = -1;
std::unordered_map<std::string, std::pair<double, int64_t>> totals;   // label -> (ns, spans)

void mark(Slot& s, VkCommandBuffer cmd, std::string label) {
    if (s.labels.size() >= kMarks) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, (uint32_t)(cur * kMarks + s.labels.size()));
    s.labels.push_back(std::move(label));
}
std::string between(Slot& s, const char* where) {
    std::string l = std::string(where) + ": " + (s.pending.empty() ? std::string("nothing named") : s.pending);
    s.pending.clear();
    return l;
}

} // namespace

bool gprof_on() {
    static const bool on = getenv("RX_VK_GPU_PROFILE") && atoi(getenv("RX_VK_GPU_PROFILE")) != 0;
    return on;
}

void gprof_begin_recording(int slot, VkCommandBuffer cmd) {
    if (!gprof_on() || slot < 0 || slot >= kSlots) return;
    if (!pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kSlots * kMarks;
        if (vkCreateQueryPool(dev.device, &qi, nullptr, &pool) != VK_SUCCESS) return;
        logf("vk: GPU profile on (RX_VK_GPU_PROFILE): spans and the time between them");
    }
    cur = slot;
    Slot& s = slots[slot];
    s.cmd = cmd;
    s.labels.clear();
    s.pending.clear();
    s.open = false;
    s.used = true;
    vkCmdResetQueryPool(cmd, pool, slot * kMarks, kMarks);
    mark(s, cmd, "");
}

void gprof_begin(VkCommandBuffer cmd) {
    if (!pool || cur < 0) return;
    Slot& s = slots[cur];
    if (s.open || cmd != s.cmd) return;
    mark(s, cmd, between(s, "between"));
    s.open = true;
}

void gprof_end(VkCommandBuffer cmd, const std::string& label) {
    if (!pool || cur < 0) return;
    Slot& s = slots[cur];
    if (!s.open || cmd != s.cmd) return;
    mark(s, cmd, label);
    s.open = false;
}

void gprof_event(VkCommandBuffer cmd, const char* what) {
    if (!pool || cur < 0) return;
    Slot& s = slots[cur];
    if (s.open || cmd != s.cmd || s.pending.size() > 120) return;   // (inside a span its cost is the span's)
    if (s.pending.find(what) != std::string::npos) return;
    if (!s.pending.empty()) s.pending += "+";
    s.pending += what;
}

void gprof_end_recording(VkCommandBuffer cmd) {
    if (!pool || cur < 0) return;
    Slot& s = slots[cur];
    if (cmd != s.cmd) return;
    if (s.open) s.open = false;   // (a span left open: its time goes to the tail)
    mark(s, cmd, between(s, "recording end"));
}

void gprof_collect(int slot) {
    if (!pool || slot < 0 || slot >= kSlots) return;
    Slot& s = slots[slot];
    if (!s.used || s.labels.size() < 2) return;
    std::vector<uint64_t> t(s.labels.size());
    if (vkGetQueryPoolResults(dev.device, pool, slot * kMarks, (uint32_t)t.size(), t.size() * 8, t.data(), 8,
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    const double period = dev.props.limits.timestampPeriod;
    for (size_t i = 1; i < s.labels.size(); i++) {
        if (t[i] < t[i - 1]) continue;
        auto& e = totals[s.labels[i]];
        e.first += (double)(t[i] - t[i - 1]) * period;
        e.second++;
    }
    s.labels.clear();
    s.used = false;
}

void gprof_report(int frames) {
    if (!pool || totals.empty()) return;
    std::vector<std::pair<std::string, std::pair<double, int64_t>>> v(totals.begin(), totals.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    double all = 0, gaps = 0;
    for (const auto& e : v) {
        all += e.second.first;
        if (!e.first.compare(0, 8, "between:") || !e.first.compare(0, 14, "recording end:")) gaps += e.second.first;
    }
    logf("vk: GPU profile: %.2f ms a frame in %zu kinds of span, %.2f ms of it between the passes; the most:", all / frames / 1e6,
         v.size(), gaps / frames / 1e6);
    for (size_t i = 0; i < v.size() && i < 30; i++)
        logf("vk:   %6.3f ms  %5.1f a frame  %s", v[i].second.first / frames / 1e6, (double)v[i].second.second / frames,
             v[i].first.c_str());
    totals.clear();
}

} // namespace mkwvk
