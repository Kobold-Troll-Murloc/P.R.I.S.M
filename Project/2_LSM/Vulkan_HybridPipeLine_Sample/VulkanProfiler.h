#pragma once

// [필수] Windows 매크로 충돌 방지
#define NOMINMAX 

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <map>
#include <algorithm>
#include <chrono>

// Visual Studio Output 출력을 위한 헤더
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

struct SectionStats {
    double totalTimeMs = 0.0;
    double minTimeMs = 999999.0;
    double maxTimeMs = 0.0;
    uint64_t count = 0;
    double movingAvgMs = 0.0;

    void update(double timeMs) {
        if (timeMs < 0.0001 || timeMs > 1000.0) return;
        totalTimeMs += timeMs;
        count++;
        minTimeMs = std::min(minTimeMs, timeMs);
        maxTimeMs = std::max(maxTimeMs, timeMs);
        if (count == 1) movingAvgMs = timeMs;
        else movingAvgMs = (movingAvgMs * 0.95) + (timeMs * 0.05);
    }
    double getAverage() const { return count > 0 ? (totalTimeMs / count) : 0.0; }
};

struct SectionData {
    std::string name;
    uint32_t startQueryIdx;
    uint32_t endQueryIdx;
    uint32_t statQueryIdx;
};

class VulkanProfiler {
public:
    bool enableConsoleOutput = false;
    bool enableVSOutput = true;

    void init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t maxQueries = 128) {
        this->device = device;
        this->physicalDevice = physicalDevice;
        this->maxQueries = maxQueries;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        timestampPeriod = props.limits.timestampPeriod;

        // 1. Timestamp Pool
        VkQueryPoolCreateInfo timePoolInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        timePoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        timePoolInfo.queryCount = maxQueries;
        if (vkCreateQueryPool(device, &timePoolInfo, nullptr, &queryPoolTimestamp) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create timestamp query pool!");
        }

        // 2. Statistics Pool
        VkQueryPoolCreateInfo statPoolInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        statPoolInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        statPoolInfo.queryCount = maxQueries;
        statPoolInfo.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT;

        if (vkCreateQueryPool(device, &statPoolInfo, nullptr, &queryPoolStats) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create statistics query pool!");
        }
    }

    void cleanup() {
        if (queryPoolTimestamp) vkDestroyQueryPool(device, queryPoolTimestamp, nullptr);
        if (queryPoolStats) vkDestroyQueryPool(device, queryPoolStats, nullptr);
    }

    void beginFrame(VkCommandBuffer cmdBuf) {
        vkCmdResetQueryPool(cmdBuf, queryPoolTimestamp, 0, maxQueries);
        vkCmdResetQueryPool(cmdBuf, queryPoolStats, 0, maxQueries);
        currentQueryIdx = 0;
        currentStatIdx = 0;
        frameDrawCalls = 0;
        frameInstanceCount = 0;
        frameDispatchCalls = 0;
        frameTraceRaysCalls = 0;
        frameSections.clear();
    }

    // ================= [래퍼 함수] =================
    void CmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t instC, uint32_t fi, int32_t vo, uint32_t fInst) {
        frameDrawCalls++; frameInstanceCount += instC;
        vkCmdDrawIndexed(cb, ic, instC, fi, vo, fInst);
    }
    void CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t instC, uint32_t fv, uint32_t fInst) {
        frameDrawCalls++; frameInstanceCount += instC;
        vkCmdDraw(cb, vc, instC, fv, fInst);
    }
    void CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
        frameDispatchCalls++; vkCmdDispatch(cb, x, y, z);
    }
    void CmdTraceRaysKHR(VkCommandBuffer cb, const VkStridedDeviceAddressRegionKHR* r, const VkStridedDeviceAddressRegionKHR* m, const VkStridedDeviceAddressRegionKHR* h, const VkStridedDeviceAddressRegionKHR* c, uint32_t w, uint32_t ht, uint32_t d) {
        frameTraceRaysCalls++;
        auto func = (PFN_vkCmdTraceRaysKHR)vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR");
        if (func) func(cb, r, m, h, c, w, ht, d);
    }

    void beginSection(VkCommandBuffer cmdBuf, const std::string& name) {
        if (currentQueryIdx + 2 > maxQueries) return;
        uint32_t idx = currentQueryIdx++;
        vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPoolTimestamp, idx);
        vkCmdBeginQuery(cmdBuf, queryPoolStats, currentStatIdx, 0);
        SectionData data; data.name = name; data.startQueryIdx = idx; data.statQueryIdx = currentStatIdx;
        frameSections.push_back(data);
    }

    void endSection(VkCommandBuffer cmdBuf) {
        if (frameSections.empty()) return;
        SectionData& data = frameSections.back();
        vkCmdEndQuery(cmdBuf, queryPoolStats, currentStatIdx);
        currentStatIdx++;
        uint32_t idx = currentQueryIdx++;
        data.endQueryIdx = idx;
        vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPoolTimestamp, idx);
    }

    void updateAndPrintConsole() {
        if (currentQueryIdx == 0) return;

        // 1. Time 결과 가져오기
        std::vector<uint64_t> timeResults(currentQueryIdx);
        VkResult resTime = vkGetQueryPoolResults(device, queryPoolTimestamp, 0, currentQueryIdx,
            sizeof(uint64_t) * currentQueryIdx, timeResults.data(), sizeof(uint64_t), // Time은 1개씩이므로 stride = sizeof(uint64) 맞음
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        // 2. Stat 결과 가져오기 (여기가 중요!)
        int numStats = 3; // Vertices, Primitives, Invocations
        std::vector<uint64_t> statResults(currentStatIdx * numStats);

        // ★★★ [버그 수정] stride를 'sizeof(uint64_t)'에서 'sizeof(uint64_t) * numStats'로 변경 ★★★
        // 쿼리 하나당 데이터 3개가 묶여있기 때문에 3칸씩 건너뛰어야 합니다.
        VkResult resStat = vkGetQueryPoolResults(device, queryPoolStats, 0, currentStatIdx,
            sizeof(uint64_t) * currentStatIdx * numStats, statResults.data(),
            sizeof(uint64_t) * numStats, // <--- [핵심 수정 사항]
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        if (resTime != VK_SUCCESS || resStat != VK_SUCCESS) return;

        double totalFrameTime = 0.0;
        for (const auto& sec : frameSections) {
            uint64_t tStart = timeResults[sec.startQueryIdx];
            uint64_t tEnd = timeResults[sec.endQueryIdx];
            if (tEnd <= tStart) continue;

            double durationMs = (double)(tEnd - tStart) * timestampPeriod / 1000000.0;
            if (durationMs > 1000.0) continue;

            statsMap[sec.name].update(durationMs);
            totalFrameTime += durationMs;
        }

        // 출력 주기
        static auto lastPrint = std::chrono::high_resolution_clock::now();
        auto now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration<float, std::milli>(now - lastPrint).count() < 500.0f) return;
        lastPrint = now;

        std::stringstream ss;
        ss << "\n___________________________________________________________________\n";
        ss << " [P.R.I.S.M] Perf Update | FPS: " << std::fixed << std::setprecision(0) << (1000.0 / (totalFrameTime > 0 ? totalFrameTime : 1.0))
            << " | GPU Time: " << std::setprecision(2) << totalFrameTime << "ms \n";

        printMemoryInfo(ss);

        ss << " [Calls] Draw: " << frameDrawCalls << " (Inst: " << frameInstanceCount << ")"
            << " | Dispatch: " << frameDispatchCalls << " | TraceRays: " << frameTraceRaysCalls << "\n";

        ss << " -------------------------------------------------------------------\n";
        ss << " " << std::left << std::setw(12) << "Section" << std::right << std::setw(9) << "Cur(ms)"
            << std::setw(9) << "Avg(ms)" << std::setw(14) << "Primitives" << "\n";
        ss << " -------------------------------------------------------------------\n";

        for (const auto& sec : frameSections) {
            const auto& stat = statsMap[sec.name];
            // 데이터가 3개씩 묶여 있으므로 인덱스 계산 주의
            uint64_t prims = statResults[sec.statQueryIdx * numStats + 1];

            ss << " " << std::left << std::setw(12) << sec.name
                << std::right << std::fixed << std::setprecision(3)
                << std::setw(9) << stat.movingAvgMs
                << std::setw(9) << stat.getAverage()
                << std::setw(14) << formatNum(prims) << "\n";
        }
        ss << "___________________________________________________________________\n";

        if (enableConsoleOutput) std::cout << "\033[H" << ss.str();
        if (enableVSOutput) OutputDebugStringA(ss.str().c_str());
    }

private:
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueryPool queryPoolTimestamp = VK_NULL_HANDLE;
    VkQueryPool queryPoolStats = VK_NULL_HANDLE;
    float timestampPeriod = 1.0f;
    uint32_t maxQueries = 0;
    uint32_t currentQueryIdx = 0;
    uint32_t currentStatIdx = 0;

    uint32_t frameDrawCalls = 0;
    uint32_t frameInstanceCount = 0;
    uint32_t frameDispatchCalls = 0;
    uint32_t frameTraceRaysCalls = 0;

    std::vector<SectionData> frameSections;
    std::map<std::string, SectionStats> statsMap;

    std::string formatNum(uint64_t num) {
        if (num > 1000000) return std::to_string(num / 1000000) + "M";
        if (num > 1000) return std::to_string(num / 1000) + "k";
        return std::to_string(num);
    }

    void printMemoryInfo(std::stringstream& ss) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT memBudget{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
        VkPhysicalDeviceMemoryProperties2 memProps2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
        memProps2.pNext = &memBudget;
        vkGetPhysicalDeviceMemoryProperties2(physicalDevice, &memProps2);

        VkDeviceSize vramUsage = 0;
        VkDeviceSize vramBudget = 0;
        for (uint32_t i = 0; i < memProps2.memoryProperties.memoryHeapCount; i++) {
            if (memProps2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                vramUsage = memBudget.heapUsage[i];
                vramBudget = memBudget.heapBudget[i];
                break;
            }
        }
        ss << " [VRAM] " << (vramUsage / 1024 / 1024) << "MB / " << (vramBudget / 1024 / 1024) << "MB\n";
    }
};